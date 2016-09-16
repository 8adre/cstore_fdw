/*-------------------------------------------------------------------------
 *
 * cstore_writer.c
 *
 * This file contains function definitions for writing cstore files. This
 * includes the logic for writing file level metadata, writing row stripes,
 * and calculating block skip nodes.
 *
 * Copyright (c) 2016, Citus Data, Inc.
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */


#include "postgres.h"
#include "cstore_fdw.h"
#include "cstore_metadata_serialization.h"

#include <sys/stat.h>
#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/nbtree.h"
#include "catalog/pg_collation.h"
#include "catalog/storage_xlog.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "optimizer/var.h"
#include "port.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#if PG_VERSION_NUM >= 90400
#define LOG_NEWPAGE_BUFFER(buffer) log_newpage_buffer(buffer, false)
#else
#define LOG_NEWPAGE_BUFFER(buffer) log_newpage_buffer(buffer)
#endif

static void EnsureDataForkExists(Relation relation, bool logging);
static void CStoreWriteFooter(TableFooter *tableFooter, Relation relation,
							  bool loggingEnabled);
static StripeBuffers * CreateEmptyStripeBuffers(uint32 stripeMaxRowCount,
												uint32 blockRowCount,
												uint32 columnCount);
static StripeSkipList * CreateEmptyStripeSkipList(uint32 stripeMaxRowCount,
												  uint32 blockRowCount,
												  uint32 columnCount);
static StripeMetadata FlushStripe(TableWriteState *writeState);
static StringInfo * CreateSkipListBufferArray(StripeSkipList *stripeSkipList,
											  TupleDesc tupleDescriptor);
static StripeFooter * CreateStripeFooter(StripeSkipList *stripeSkipList,
										 StringInfo *skipListBufferArray);
static StringInfo SerializeBoolArray(bool *boolArray, uint32 boolArrayLength);
static void SerializeSingleDatum(StringInfo datumBuffer, Datum datum,
								 bool datumTypeByValue, int datumTypeLength,
								 char datumTypeAlign);
static void SerializeBlockData(TableWriteState *writeState, uint32 blockIndex,
							   uint32 rowCount);
static void UpdateBlockSkipNodeMinMax(ColumnBlockSkipNode *blockSkipNode,
									  Datum columnValue, bool columnTypeByValue,
									  int columnTypeLength, Oid columnCollation,
									  FmgrInfo *comparisonFunction);
static Datum DatumCopy(Datum datum, bool datumTypeByValue, int datumTypeLength);
static void AppendStripeMetadata(TableFooter *tableFooter,
								 StripeMetadata stripeMetadata);
static void WriteToFile(TableWriteState *writeState, void *data, uint32 dataLength);
static StringInfo CopyStringInfo(StringInfo sourceString);


/*
 * CStoreBeginWrite initializes a cstore data load operation and returns a table
 * handle. This handle should be used for adding the row values and finishing the
 * data load operation. If the cstore footer data already exists, we read the
 * footer and then seek to right after the last stripe  where the new stripes
 * will be added.
 */
TableWriteState *
CStoreBeginWrite(Relation relation, CompressionType compressionType,
				 uint64 stripeMaxRowCount, uint32 blockRowCount,
				 bool logging, TupleDesc tupleDescriptor)
{
	TableWriteState *writeState = NULL;
	TableFooter *tableFooter = NULL;
	FmgrInfo **comparisonFunctionArray = NULL;
	MemoryContext stripeWriteContext = NULL;
	uint64 currentFileOffset = 0;
	uint32 columnCount = 0;
	uint32 columnIndex = 0;
	bool *columnMaskArray = NULL;
	ColumnBlockData **blockData = NULL;
	int32 pageDataSize = CSTORE_PAGE_DATA_SIZE;
	int32 blockNumber = 0;

	tableFooter = CStoreReadFooter(relation);

	if (tableFooter == NULL)
	{
		tableFooter = palloc0(sizeof(TableFooter));
		tableFooter->blockRowCount = blockRowCount;
		tableFooter->stripeMetadataList = NIL;
	}

	/*
	 * If stripeMetadataList is not empty, jump to the position right after
	 * the last position.
	 */
	if (tableFooter->stripeMetadataList != NIL)
	{
		StripeMetadata *lastStripe = NULL;
		uint64 lastStripeSize = 0;

		lastStripe = llast(tableFooter->stripeMetadataList);
		lastStripeSize += lastStripe->skipListLength;
		lastStripeSize += lastStripe->dataLength;
		lastStripeSize += lastStripe->footerLength;

		currentFileOffset = lastStripe->fileOffset + lastStripeSize;
	}

	blockNumber = currentFileOffset / pageDataSize;

	/* get comparison function pointers for each of the columns */
	columnCount = tupleDescriptor->natts;
	comparisonFunctionArray = palloc0(columnCount * sizeof(FmgrInfo *));
	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		FmgrInfo *comparisonFunction = NULL;
		FormData_pg_attribute *attributeForm = tupleDescriptor->attrs[columnIndex];

		if (!attributeForm->attisdropped)
		{
			Oid typeId = attributeForm->atttypid;

			comparisonFunction = GetFunctionInfoOrNull(typeId, BTREE_AM_OID, BTORDER_PROC);
		}

		comparisonFunctionArray[columnIndex] = comparisonFunction;
	}



	/*
	 * We allocate all stripe specific data in the stripeWriteContext, and
	 * reset this memory context once we have flushed the stripe to the file.
	 * This is to avoid memory leaks.
	 */
	stripeWriteContext = AllocSetContextCreate(CurrentMemoryContext,
											   "Stripe Write Memory Context",
											   ALLOCSET_DEFAULT_MINSIZE,
											   ALLOCSET_DEFAULT_INITSIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);

	/* make sure file for data storage exists */
	EnsureDataForkExists(relation, logging);

	columnMaskArray = palloc(columnCount * sizeof(bool));
	memset(columnMaskArray, true, columnCount);

	blockData = CreateEmptyBlockDataArray(columnCount, columnMaskArray, blockRowCount);

	writeState = palloc0(sizeof(TableWriteState));
	writeState->tableFooter = tableFooter;
	writeState->compressionType = compressionType;
	writeState->stripeMaxRowCount = stripeMaxRowCount;
	writeState->logging = logging;
	writeState->tupleDescriptor = tupleDescriptor;
	writeState->currentFileOffset = currentFileOffset;
	writeState->comparisonFunctionArray = comparisonFunctionArray;
	writeState->stripeBuffers = NULL;
	writeState->stripeSkipList = NULL;
	writeState->stripeWriteContext = stripeWriteContext;
	writeState->blockDataArray = blockData;
	writeState->compressionBuffer = NULL;
	writeState->activeBlockNumber = blockNumber;
	writeState->relation = relation;

	return writeState;
}


/*
 * EnsureDataForkExists checks if data fork exists for writing data,
 * and creates if it is not present.
 */
static void EnsureDataForkExists(Relation relation, bool logging)
{
	SMgrRelation srel = smgropen(relation->rd_node, InvalidBackendId);

	if (!smgrexists(srel, DATA_FORKNUM))
	{
		smgrcreate(srel, DATA_FORKNUM, false);

		if (logging)
		{
			log_smgrcreate(&srel->smgr_rnode.node, DATA_FORKNUM);
		}
	}

	Assert(smgrexists(srel, DATA_FORKNUM));
	smgrclose(srel);
}


/*
 * CStoreWriteRow adds a row to the cstore file. If the stripe is not initialized,
 * we create structures to hold stripe data and skip list. Then, we serialize and
 * append data to serialized value buffer for each of the columns and update
 * corresponding skip nodes. Then, whole block data is compressed at every
 * rowBlockCount insertion. Then, if row count exceeds stripeMaxRowCount, we flush
 * the stripe, and add its metadata to the table footer.
 */
void
CStoreWriteRow(TableWriteState *writeState, Datum *columnValues, bool *columnNulls)
{
	uint32 columnIndex = 0;
	uint32 blockIndex = 0;
	uint32 blockRowIndex = 0;
	StripeBuffers *stripeBuffers = writeState->stripeBuffers;
	StripeSkipList *stripeSkipList = writeState->stripeSkipList;
	uint32 columnCount = writeState->tupleDescriptor->natts;
	TableFooter *tableFooter = writeState->tableFooter;
	const uint32 blockRowCount = tableFooter->blockRowCount;
	ColumnBlockData **blockDataArray = writeState->blockDataArray;
	MemoryContext oldContext = MemoryContextSwitchTo(writeState->stripeWriteContext);

	if (stripeBuffers == NULL)
	{
		stripeBuffers = CreateEmptyStripeBuffers(writeState->stripeMaxRowCount,
												 blockRowCount, columnCount);
		stripeSkipList = CreateEmptyStripeSkipList(writeState->stripeMaxRowCount,
												   blockRowCount, columnCount);
		writeState->stripeBuffers = stripeBuffers;
		writeState->stripeSkipList = stripeSkipList;
		writeState->compressionBuffer = makeStringInfo();

		/*
		 * serializedValueBuffer lives in stripe write memory context so it needs to be
		 * initialized when the stripe is created.
		 */
		for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
		{
			ColumnBlockData *blockData = blockDataArray[columnIndex];
			blockData->valueBuffer = makeStringInfo();
		}
	}

	blockIndex = stripeBuffers->rowCount / blockRowCount;
	blockRowIndex = stripeBuffers->rowCount % blockRowCount;

	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		ColumnBlockData *blockData = blockDataArray[columnIndex];
		ColumnBlockSkipNode **blockSkipNodeArray = stripeSkipList->blockSkipNodeArray;
		ColumnBlockSkipNode *blockSkipNode =
			&blockSkipNodeArray[columnIndex][blockIndex];

		if (columnNulls[columnIndex])
		{
			blockData->existsArray[blockRowIndex] = false;
		}
		else
		{
			FmgrInfo *comparisonFunction =
				writeState->comparisonFunctionArray[columnIndex];
			Form_pg_attribute attributeForm =
				writeState->tupleDescriptor->attrs[columnIndex];
			bool columnTypeByValue = attributeForm->attbyval;
			int columnTypeLength = attributeForm->attlen;
			Oid columnCollation = attributeForm->attcollation;
			char columnTypeAlign  = attributeForm->attalign;

			blockData->existsArray[blockRowIndex] = true;

			SerializeSingleDatum(blockData->valueBuffer, columnValues[columnIndex],
								 columnTypeByValue, columnTypeLength, columnTypeAlign);

			UpdateBlockSkipNodeMinMax(blockSkipNode, columnValues[columnIndex],
									  columnTypeByValue, columnTypeLength,
									  columnCollation, comparisonFunction);
		}

		blockSkipNode->rowCount++;
	}

	stripeSkipList->blockCount = blockIndex + 1;

	/* last row of the block is inserted serialize the block */
	if (blockRowIndex == blockRowCount - 1)
	{
		SerializeBlockData(writeState, blockIndex, blockRowCount);
	}

	stripeBuffers->rowCount++;
	if (stripeBuffers->rowCount >= writeState->stripeMaxRowCount)
	{
		StripeMetadata stripeMetadata = FlushStripe(writeState);
		MemoryContextReset(writeState->stripeWriteContext);

		/* set stripe data and skip list to NULL so they are recreated next time */
		writeState->stripeBuffers = NULL;
		writeState->stripeSkipList = NULL;

		/*
		 * Append stripeMetadata in old context so next MemoryContextReset
		 * doesn't free it.
		 */
		MemoryContextSwitchTo(oldContext);
		AppendStripeMetadata(tableFooter, stripeMetadata);
	}
	else
	{
		MemoryContextSwitchTo(oldContext);
	}
}


/*
 * CStoreEndWrite finishes a cstore data load operation. If we have an unflushed
 * stripe, we flush it. Then, we sync and close the cstore data file. Last, we
 * flush the footer to a temporary file, and atomically rename this temporary
 * file to the original footer file.
 */
void
CStoreEndWrite(TableWriteState *writeState)
{
	int columnCount = writeState->tupleDescriptor->natts;
	StripeBuffers *stripeBuffers = writeState->stripeBuffers;

	if (stripeBuffers != NULL)
	{
		MemoryContext oldContext = MemoryContextSwitchTo(writeState->stripeWriteContext);
		StripeMetadata stripeMetadata = FlushStripe(writeState);

		MemoryContextReset(writeState->stripeWriteContext);

		MemoryContextSwitchTo(oldContext);
		AppendStripeMetadata(writeState->tableFooter, stripeMetadata);
	}

	CStoreWriteFooter(writeState->tableFooter, writeState->relation,
					  writeState->logging);

	MemoryContextDelete(writeState->stripeWriteContext);
	list_free_deep(writeState->tableFooter->stripeMetadataList);
	pfree(writeState->tableFooter);
	pfree(writeState->comparisonFunctionArray);
	FreeColumnBlockDataArray(writeState->blockDataArray, columnCount);
	pfree(writeState);
}


/*
 * CStoreWriteFooter writes the given footer data to relation footer file.
 * First the function serializes the footer, the postscript, and the the
 * postscript size as the last byte of the footer data.
 * After preparing the footer data the function reads the current footer
 * metadata to decide where to write to make sure that the current footer data
 * is not overwritten. It writes the footer data to correct place and finally
 * updates footer metadata about where footer data is stored.
 */
static void
CStoreWriteFooter(TableFooter *tableFooter, Relation relation, bool loggingEnabled)
{
	StringInfo tableFooterBuffer = NULL;
	StringInfo postscriptBuffer = NULL;
	uint8 postscriptSize = 0;
	BlockNumber originalBlockCount = 0;
	BlockNumber actualBlockCount = 0;
	BlockNumber currentBlockNumber = 0;
	StringInfo wholeFooter = makeStringInfo();
	int32 dataLength = 0;
	int32 dataOffset = 0;
	int32 blockDataSize =  CSTORE_PAGE_DATA_SIZE;
	BlockNumber headerBlockNumber = InvalidBlockNumber;
	Buffer headerBuffer = InvalidBuffer;
	Page headerPage = NULL;
	PageHeader headerBufferPageHeader = NULL;
	char *headerPageContent = NULL;
	uint32 startingBlock = InvalidBlockNumber;
	uint32 blockCount = InvalidBlockNumber;
	uint32 newStartingBlock = InvalidBlockNumber;
	StringInfo tableFooterMetadata = NULL;

	originalBlockCount = RelationGetNumberOfBlocksInFork(relation, FOOTER_FORKNUM);

	appendBinaryStringInfo(wholeFooter, (const char *) &dataLength, sizeof(int32));
	/* write the footer */
	tableFooterBuffer = SerializeTableFooter(tableFooter);
	appendBinaryStringInfo(wholeFooter, tableFooterBuffer->data, tableFooterBuffer->len);

	/* write the postscript */
	postscriptBuffer = SerializePostScript(tableFooterBuffer->len);
	appendBinaryStringInfo(wholeFooter, postscriptBuffer->data, postscriptBuffer->len);

	/* write the 1-byte postscript size */
	Assert(postscriptBuffer->len < CSTORE_POSTSCRIPT_SIZE_MAX);
	postscriptSize = postscriptBuffer->len;
	appendBinaryStringInfo(wholeFooter, (char *) &postscriptSize, CSTORE_POSTSCRIPT_SIZE_LENGTH);

	dataLength = wholeFooter->len;
	actualBlockCount = (dataLength - 1) / blockDataSize + 1;

	if (originalBlockCount > 0)
	{
		headerBlockNumber = 0;
	}
	else
	{
		headerBlockNumber = P_NEW;
	}

	headerBuffer = ReadBufferExtended(relation, FOOTER_FORKNUM, headerBlockNumber, RBM_NORMAL, NULL);

	LockBuffer(headerBuffer, BUFFER_LOCK_EXCLUSIVE);

	headerPage = BufferGetPage(headerBuffer);
	headerBufferPageHeader = (PageHeader) headerPage;
	headerPageContent = PageGetContents(headerPage);

	/*
	 * if we have nothing to read, we start from the first available buffer (1),
	 * otherwise we first read metadata header buffer and compute the starting buffer
	 * number.
	 */
	if (originalBlockCount == 0)
	{
		newStartingBlock = 1;
	}
	else
	{
		uint32 headerDataLength = headerBufferPageHeader->pd_lower - SizeOfPageHeaderData;
		DeserializeTableFooterMetadata(headerPageContent, headerDataLength, &startingBlock, &blockCount);

		/*
		 * We here we decide where to start writing new table footer metadata.
		 * If deserialization fail for any reason starting block is set to an invalid
		 * starting block number (0).
		 *
		 * if there is a parsing error we start from the first buffer (1). If it is fine
		 * we check if there are enough empty buffers to accommodate all blocks of
		 * table footer metadata and set new starting buffer to first buffer (1).
		 * If there are not enough buffers we start writing after the all used buffers.
		 */
		if (startingBlock == 0)
		{
			newStartingBlock = 1;
		}
		else if (actualBlockCount < startingBlock)
		{
			newStartingBlock = 1;
		}
		else
		{
			newStartingBlock = startingBlock + blockCount;
		}
	}

	dataOffset = 0;
	memcpy(wholeFooter->data, (const char*) &dataLength, sizeof(int32));

	for (currentBlockNumber = 0; currentBlockNumber < actualBlockCount; currentBlockNumber++)
	{
		Buffer buffer = InvalidBuffer;
		BlockNumber blockNumber = currentBlockNumber + newStartingBlock;
		Page page;
		PageHeader pageHeader = NULL;
		char * pageData = NULL;
		BlockNumber actualBlockNumber = InvalidBlockNumber;


		if (blockNumber >= originalBlockCount)
		{
			blockNumber = P_NEW;
		}

		buffer = ReadBufferExtended(relation, FOOTER_FORKNUM, blockNumber, RBM_NORMAL, NULL);

		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

		START_CRIT_SECTION();
		page = BufferGetPage(buffer);

		PageInit(page, BLCKSZ, 0);
		pageData = PageGetContents(page);

		int copySize = blockDataSize;

		if ( (dataLength - dataOffset) <= blockDataSize)
		{
			copySize = dataLength - dataOffset;
		}

		pageHeader = (PageHeader) page;
		pageHeader->pd_lower = SizeOfPageHeaderData + copySize;
		memcpy(pageData, wholeFooter->data + dataOffset, copySize);

		MarkBufferDirty(buffer);
		actualBlockNumber = BufferGetBlockNumber(buffer);
		if (loggingEnabled)
		{
			LOG_NEWPAGE_BUFFER(buffer);
		}

		END_CRIT_SECTION();

		UnlockReleaseBuffer(buffer);

		dataOffset += copySize;

	}

	/* all table footer is written, update the header page */
	tableFooterMetadata = SerializeTableFooterMetadata(newStartingBlock, actualBlockCount);

	START_CRIT_SECTION();
	PageInit(headerPage, BLCKSZ, 0);
	memcpy(headerPageContent, tableFooterMetadata->data, tableFooterMetadata->len);
	headerBufferPageHeader->pd_lower = SizeOfPageHeaderData + tableFooterMetadata->len;
	MarkBufferDirty(headerBuffer);

	/*
	 * Changes to headerBuffer is logged regardless of logging setting to enable
	 * recovery after crash.
	 */
	LOG_NEWPAGE_BUFFER(headerBuffer);

	END_CRIT_SECTION();

	UnlockReleaseBuffer(headerBuffer);

	pfree(tableFooterBuffer->data);
	pfree(tableFooterBuffer);
	pfree(postscriptBuffer->data);
	pfree(postscriptBuffer);

	pfree(wholeFooter->data);
	pfree(wholeFooter);

	pfree(tableFooterMetadata->data);
	pfree(tableFooterMetadata);
}


/*
 * CreateEmptyStripeBuffers allocates an empty StripeBuffers structure with the given
 * column count.
 */
static StripeBuffers *
CreateEmptyStripeBuffers(uint32 stripeMaxRowCount, uint32 blockRowCount,
						 uint32 columnCount)
{
	StripeBuffers *stripeBuffers = NULL;
	uint32 columnIndex = 0;
	uint32 maxBlockCount = (stripeMaxRowCount / blockRowCount) + 1;
	ColumnBuffers **columnBuffersArray = palloc0(columnCount * sizeof(ColumnBuffers *));

	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		uint32 blockIndex = 0;
		ColumnBlockBuffers **blockBuffersArray =
			palloc0(maxBlockCount * sizeof(ColumnBlockBuffers *));

		for (blockIndex = 0; blockIndex < maxBlockCount; blockIndex++)
		{
			blockBuffersArray[blockIndex] = palloc0(sizeof(ColumnBlockBuffers));
			blockBuffersArray[blockIndex]->existsBuffer = NULL;
			blockBuffersArray[blockIndex]->valueBuffer = NULL;
			blockBuffersArray[blockIndex]->valueCompressionType = COMPRESSION_NONE;
		}

		columnBuffersArray[columnIndex] = palloc0(sizeof(ColumnBuffers));
		columnBuffersArray[columnIndex]->blockBuffersArray = blockBuffersArray;
	}

	stripeBuffers = palloc0(sizeof(StripeBuffers));
	stripeBuffers->columnBuffersArray = columnBuffersArray;
	stripeBuffers->columnCount = columnCount;
	stripeBuffers->rowCount = 0;

	return stripeBuffers;
}


/*
 * CreateEmptyStripeSkipList allocates an empty StripeSkipList structure with
 * the given column count. This structure has enough blocks to hold statistics
 * for stripeMaxRowCount rows.
 */
static StripeSkipList *
CreateEmptyStripeSkipList(uint32 stripeMaxRowCount, uint32 blockRowCount,
						  uint32 columnCount)
{
	StripeSkipList *stripeSkipList = NULL;
	uint32 columnIndex = 0;
	uint32 maxBlockCount = (stripeMaxRowCount / blockRowCount) + 1;

	ColumnBlockSkipNode **blockSkipNodeArray =
		palloc0(columnCount * sizeof(ColumnBlockSkipNode *));
	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		blockSkipNodeArray[columnIndex] =
			palloc0(maxBlockCount * sizeof(ColumnBlockSkipNode));
	}

	stripeSkipList = palloc0(sizeof(StripeSkipList));
	stripeSkipList->columnCount = columnCount;
	stripeSkipList->blockCount = 0;
	stripeSkipList->blockSkipNodeArray = blockSkipNodeArray;

	return stripeSkipList;
}


/*
 * FlushStripe flushes current stripe data into the file. The function first ensures
 * the last data block for each column is properly serialized and compressed. Then, 
 * the function creates the skip list and footer buffers. Finally, the function
 * flushes the skip list, data, and footer buffers to the file.
 */
static StripeMetadata
FlushStripe(TableWriteState *writeState)
{
	StripeMetadata stripeMetadata = {0, 0, 0, 0};
	uint64 skipListLength = 0;
	uint64 dataLength = 0;
	StringInfo *skipListBufferArray = NULL;
	StripeFooter *stripeFooter = NULL;
	StringInfo stripeFooterBuffer = NULL;
	uint32 columnIndex = 0;
	uint32 blockIndex = 0;
	TableFooter *tableFooter = writeState->tableFooter;
	StripeBuffers *stripeBuffers = writeState->stripeBuffers;
	StripeSkipList *stripeSkipList = writeState->stripeSkipList;
	ColumnBlockSkipNode **columnSkipNodeArray = stripeSkipList->blockSkipNodeArray;
	TupleDesc tupleDescriptor = writeState->tupleDescriptor;
	uint32 columnCount = tupleDescriptor->natts;
	uint32 blockCount = stripeSkipList->blockCount;
	uint32 blockRowCount = tableFooter->blockRowCount;
	uint32 lastBlockIndex = stripeBuffers->rowCount / blockRowCount;
	uint32 lastBlockRowCount = stripeBuffers->rowCount % blockRowCount;

	/*
	 * check if the last block needs serialization , the last block was not serialized
	 * if it was not full yet, e.g.  (rowCount > 0)
	 */
	if (lastBlockRowCount > 0)
	{
		SerializeBlockData(writeState, lastBlockIndex, lastBlockRowCount);
	}

	/* update buffer sizes and positions in stripe skip list */
	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		ColumnBlockSkipNode *blockSkipNodeArray = columnSkipNodeArray[columnIndex];
		uint64 currentExistsBlockOffset = 0;
		uint64 currentValueBlockOffset = 0;
		ColumnBuffers *columnBuffers = stripeBuffers->columnBuffersArray[columnIndex];

		for (blockIndex = 0; blockIndex < blockCount; blockIndex++)
		{
			ColumnBlockBuffers *blockBuffers =
					columnBuffers->blockBuffersArray[blockIndex];
			uint64 existsBufferSize = blockBuffers->existsBuffer->len;
			uint64 valueBufferSize = blockBuffers->valueBuffer->len;
			CompressionType valueCompressionType = blockBuffers->valueCompressionType;
			ColumnBlockSkipNode *blockSkipNode = &blockSkipNodeArray[blockIndex];

			blockSkipNode->existsBlockOffset = currentExistsBlockOffset;
			blockSkipNode->existsLength = existsBufferSize;
			blockSkipNode->valueBlockOffset = currentValueBlockOffset;
			blockSkipNode->valueLength = valueBufferSize;
			blockSkipNode->valueCompressionType = valueCompressionType;

			currentExistsBlockOffset += existsBufferSize;
			currentValueBlockOffset += valueBufferSize;
		}
	}

	/* create skip list and footer buffers */
	skipListBufferArray = CreateSkipListBufferArray(stripeSkipList, tupleDescriptor);
	stripeFooter = CreateStripeFooter(stripeSkipList, skipListBufferArray);
	stripeFooterBuffer = SerializeStripeFooter(stripeFooter);

	/*
	 * Each stripe has three sections:
	 * (1) Skip list, which contains statistics for each column block, and can
	 * be used to skip reading row blocks that are refuted by WHERE clause list,
	 * (2) Data section, in which we store data for each column continuously.
	 * We store data for each for each column in blocks. For each block, we
	 * store two buffers: "exists" buffer, and "value" buffer. "exists" buffer
	 * tells which values are not NULL. "value" buffer contains values for
	 * present values. For each column, we first store all "exists" buffers,
	 * and then all "value" buffers.
	 * (3) Stripe footer, which contains the skip list buffer size, exists buffer
	 * size, and value buffer size for each of the columns.
	 *
	 * We start by flushing the skip list buffers.
	 */
	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		StringInfo skipListBuffer = skipListBufferArray[columnIndex];
		WriteToFile(writeState, skipListBuffer->data, skipListBuffer->len);
	}

	/* then, we flush the data buffers */
	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		ColumnBuffers *columnBuffers = stripeBuffers->columnBuffersArray[columnIndex];
		uint32 blockIndex = 0;

		for (blockIndex = 0; blockIndex < stripeSkipList->blockCount; blockIndex++)
		{
			ColumnBlockBuffers *blockBuffers =
					columnBuffers->blockBuffersArray[blockIndex];
			StringInfo existsBuffer = blockBuffers->existsBuffer;

			WriteToFile(writeState, existsBuffer->data, existsBuffer->len);
		}

		for (blockIndex = 0; blockIndex < stripeSkipList->blockCount; blockIndex++)
		{
			ColumnBlockBuffers *blockBuffers =
					columnBuffers->blockBuffersArray[blockIndex];
			StringInfo valueBuffer = blockBuffers->valueBuffer;

			WriteToFile(writeState, valueBuffer->data, valueBuffer->len);
		}
	}

	/* finally, we flush the footer buffer */
	WriteToFile(writeState, stripeFooterBuffer->data, stripeFooterBuffer->len);

	/* set stripe metadata */
	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		skipListLength += stripeFooter->skipListSizeArray[columnIndex];
		dataLength += stripeFooter->existsSizeArray[columnIndex];
		dataLength += stripeFooter->valueSizeArray[columnIndex];
	}

	stripeMetadata.fileOffset = writeState->currentFileOffset;
	stripeMetadata.skipListLength = skipListLength;
	stripeMetadata.dataLength = dataLength;
	stripeMetadata.footerLength = stripeFooterBuffer->len;

	/* advance current file offset */
	writeState->currentFileOffset += skipListLength;
	writeState->currentFileOffset += dataLength;
	writeState->currentFileOffset += stripeFooterBuffer->len;

	return stripeMetadata;
}


/*
 * CreateSkipListBufferArray serializes the skip list for each column of the
 * given stripe and returns the result as an array.
 */
static StringInfo *
CreateSkipListBufferArray(StripeSkipList *stripeSkipList, TupleDesc tupleDescriptor)
{
	StringInfo *skipListBufferArray = NULL;
	uint32 columnIndex = 0;
	uint32 columnCount = stripeSkipList->columnCount;

	skipListBufferArray = palloc0(columnCount * sizeof(StringInfo));
	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		StringInfo skipListBuffer = NULL;
		ColumnBlockSkipNode *blockSkipNodeArray =
			stripeSkipList->blockSkipNodeArray[columnIndex];
		Form_pg_attribute attributeForm = tupleDescriptor->attrs[columnIndex];

		skipListBuffer = SerializeColumnSkipList(blockSkipNodeArray,
												 stripeSkipList->blockCount,
												 attributeForm->attbyval,
												 attributeForm->attlen);

		skipListBufferArray[columnIndex] = skipListBuffer;
	}

	return skipListBufferArray;
}


/* Creates and returns the footer for given stripe. */
static StripeFooter *
CreateStripeFooter(StripeSkipList *stripeSkipList, StringInfo *skipListBufferArray)
{
	StripeFooter *stripeFooter = NULL;
	uint32 columnIndex = 0;
	uint32 columnCount = stripeSkipList->columnCount;
	uint64 *skipListSizeArray = palloc0(columnCount * sizeof(uint64));
	uint64 *existsSizeArray = palloc0(columnCount * sizeof(uint64));
	uint64 *valueSizeArray = palloc0(columnCount * sizeof(uint64));

	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		ColumnBlockSkipNode *blockSkipNodeArray =
			stripeSkipList->blockSkipNodeArray[columnIndex];
		uint32 blockIndex = 0;

		for (blockIndex = 0; blockIndex < stripeSkipList->blockCount; blockIndex++)
		{
			existsSizeArray[columnIndex] += blockSkipNodeArray[blockIndex].existsLength;
			valueSizeArray[columnIndex] += blockSkipNodeArray[blockIndex].valueLength;
		}
		skipListSizeArray[columnIndex] = skipListBufferArray[columnIndex]->len;
	}

	stripeFooter = palloc0(sizeof(StripeFooter));
	stripeFooter->columnCount = columnCount;
	stripeFooter->skipListSizeArray = skipListSizeArray;
	stripeFooter->existsSizeArray = existsSizeArray;
	stripeFooter->valueSizeArray = valueSizeArray;

	return stripeFooter;
}


/*
 * SerializeBoolArray serializes the given boolean array and returns the result
 * as a StringInfo. This function packs every 8 boolean values into one byte.
 */
static StringInfo
SerializeBoolArray(bool *boolArray, uint32 boolArrayLength)
{
	StringInfo boolArrayBuffer = NULL;
	uint32 boolArrayIndex = 0;
	uint32 byteCount = (boolArrayLength + 7) / 8;

	boolArrayBuffer = makeStringInfo();
	enlargeStringInfo(boolArrayBuffer, byteCount);
	boolArrayBuffer->len = byteCount;
	memset(boolArrayBuffer->data, 0, byteCount);

	for (boolArrayIndex = 0; boolArrayIndex < boolArrayLength; boolArrayIndex++)
	{
		if (boolArray[boolArrayIndex])
		{
			uint32 byteIndex = boolArrayIndex / 8;
			uint32 bitIndex = boolArrayIndex % 8;
			boolArrayBuffer->data[byteIndex] |= (1 << bitIndex);
		}
	}

	return boolArrayBuffer;
}


/*
 * SerializeSingleDatum serializes the given datum value and appends it to the
 * provided string info buffer.
 */
static void
SerializeSingleDatum(StringInfo datumBuffer, Datum datum, bool datumTypeByValue,
					 int datumTypeLength, char datumTypeAlign)
{
	uint32 datumLength = att_addlength_datum(0, datumTypeLength, datum);
	uint32 datumLengthAligned = att_align_nominal(datumLength, datumTypeAlign);
	char *currentDatumDataPointer = NULL;

	enlargeStringInfo(datumBuffer, datumLengthAligned);

	currentDatumDataPointer = datumBuffer->data + datumBuffer->len;
	memset(currentDatumDataPointer, 0, datumLengthAligned);

	if (datumTypeLength > 0)
	{
		if (datumTypeByValue)
		{
			store_att_byval(currentDatumDataPointer, datum, datumTypeLength);
		}
		else
		{
			memcpy(currentDatumDataPointer, DatumGetPointer(datum), datumTypeLength);
		}
	}
	else
	{
		Assert(!datumTypeByValue);
		memcpy(currentDatumDataPointer, DatumGetPointer(datum), datumLength);
	}
	
	datumBuffer->len += datumLengthAligned;
}


/*
 * SerializeBlockData serializes and compresses block data at given block index with given
 * compression type for every column.
 */
static void
SerializeBlockData(TableWriteState *writeState, uint32 blockIndex, uint32 rowCount)
{
	uint32 columnIndex = 0;
	StripeBuffers *stripeBuffers = writeState->stripeBuffers;
	ColumnBlockData **blockDataArray = writeState->blockDataArray;
	CompressionType requestedCompressionType = writeState->compressionType;
	const uint32 columnCount = stripeBuffers->columnCount;
	StringInfo compressionBuffer = writeState->compressionBuffer;

	/* serialize exist values, data values are already serialized */
	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		ColumnBuffers *columnBuffers = stripeBuffers->columnBuffersArray[columnIndex];
		ColumnBlockBuffers *blockBuffers = columnBuffers->blockBuffersArray[blockIndex];
		ColumnBlockData *blockData = blockDataArray[columnIndex];

		blockBuffers->existsBuffer = SerializeBoolArray(blockData->existsArray, rowCount);
	}

	/*
	 * check and compress value buffers, if a value buffer is not compressable
	 * then keep it as uncompressed, store compression information.
	 */
	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		ColumnBuffers *columnBuffers = stripeBuffers->columnBuffersArray[columnIndex];
		ColumnBlockBuffers *blockBuffers = columnBuffers->blockBuffersArray[blockIndex];
		ColumnBlockData *blockData = blockDataArray[columnIndex];
		StringInfo serializedValueBuffer = NULL;
		CompressionType actualCompressionType = COMPRESSION_NONE;
		bool compressed = false;

		serializedValueBuffer = blockData->valueBuffer;

		/* the only other supported compression type is pg_lz for now */
		Assert(requestedCompressionType == COMPRESSION_NONE ||
			   requestedCompressionType == COMPRESSION_PG_LZ);

		/*
		 * if serializedValueBuffer is be compressed, update serializedValueBuffer
		 * with compressed data and store compression type.
		 */
		compressed = CompressBuffer(serializedValueBuffer, compressionBuffer,
									requestedCompressionType);
		if (compressed)
		{
			serializedValueBuffer = compressionBuffer;
			actualCompressionType = COMPRESSION_PG_LZ;
		}

		/* store (compressed) value buffer */
		blockBuffers->valueCompressionType = actualCompressionType;
		blockBuffers->valueBuffer = CopyStringInfo(serializedValueBuffer);

		/* valueBuffer needs to be reset for next block's data */
		resetStringInfo(blockData->valueBuffer);
	}
}


/*
 * UpdateBlockSkipNodeMinMax takes the given column value, and checks if this
 * value falls outside the range of minimum/maximum values of the given column
 * block skip node. If it does, the function updates the column block skip node
 * accordingly.
 */
static void
UpdateBlockSkipNodeMinMax(ColumnBlockSkipNode *blockSkipNode, Datum columnValue,
						  bool columnTypeByValue, int columnTypeLength,
						  Oid columnCollation, FmgrInfo *comparisonFunction)
{
	bool hasMinMax = blockSkipNode->hasMinMax;
	Datum previousMinimum = blockSkipNode->minimumValue;
	Datum previousMaximum = blockSkipNode->maximumValue;
	Datum currentMinimum = 0;
	Datum currentMaximum = 0;

	/* if type doesn't have a comparison function, skip min/max values */
	if (comparisonFunction == NULL)
	{
		return;
	}

	if (!hasMinMax)
	{
		currentMinimum = DatumCopy(columnValue, columnTypeByValue, columnTypeLength);
		currentMaximum = DatumCopy(columnValue, columnTypeByValue, columnTypeLength);
	}
	else
	{
		Datum minimumComparisonDatum = FunctionCall2Coll(comparisonFunction,
														 columnCollation, columnValue,
														 previousMinimum);
		Datum maximumComparisonDatum = FunctionCall2Coll(comparisonFunction,
														 columnCollation, columnValue,
														 previousMaximum);
		int minimumComparison = DatumGetInt32(minimumComparisonDatum);
		int maximumComparison = DatumGetInt32(maximumComparisonDatum);

		if (minimumComparison < 0)
		{
			currentMinimum = DatumCopy(columnValue, columnTypeByValue, columnTypeLength);
		}
		else
		{
			currentMinimum = previousMinimum;
		}

		if (maximumComparison > 0)
		{
			currentMaximum = DatumCopy(columnValue, columnTypeByValue, columnTypeLength);
		}
		else
		{
			currentMaximum = previousMaximum;
		}
	}

	blockSkipNode->hasMinMax = true;
	blockSkipNode->minimumValue = currentMinimum;
	blockSkipNode->maximumValue = currentMaximum;
}


/* Creates a copy of the given datum. */
static Datum
DatumCopy(Datum datum, bool datumTypeByValue, int datumTypeLength)
{
	Datum datumCopy = 0;

	if (datumTypeByValue)
	{
		datumCopy = datum;
	}
	else
	{
		uint32 datumLength = att_addlength_datum(0, datumTypeLength, datum);
		char *datumData = palloc0(datumLength);
		memcpy(datumData, DatumGetPointer(datum), datumLength);

		datumCopy = PointerGetDatum(datumData);
	}

	return datumCopy;
}


/*
 * AppendStripeMetadata adds a copy of given stripeMetadata to the given
 * table footer's stripeMetadataList.
 */
static void
AppendStripeMetadata(TableFooter *tableFooter, StripeMetadata stripeMetadata)
{
	StripeMetadata *stripeMetadataCopy = palloc0(sizeof(StripeMetadata));
	memcpy(stripeMetadataCopy, &stripeMetadata, sizeof(StripeMetadata));

	tableFooter->stripeMetadataList = lappend(tableFooter->stripeMetadataList,
											  stripeMetadataCopy);
}


/*
 * WriteToFile appends provided data to the active page. If data does
 * not fit into remaining space in page, a new page is allocated until whole
 * data is written.
 */
static void
WriteToFile(TableWriteState *writeState, void *data, uint32 dataLength)
{
	int32 blockNumber = writeState->activeBlockNumber;
	int dataOffset = 0;
	Relation relation = writeState->relation;
	BlockNumber blockCount = RelationGetNumberOfBlocksInFork(relation, DATA_FORKNUM);

	if (dataLength == 0)
	{
		return;
	}

	while (dataOffset < dataLength)
	{
		Page page = NULL;
		char *pageData = NULL;
		Buffer buffer = InvalidBuffer;
		int32 copySize = 0;
		int pageOffset = 0;
		int remainingCapacity = 0;
		PageHeader pageHeader = NULL;

		if (blockNumber >= blockCount)
		{
			blockNumber = P_NEW;
		}

		buffer = ReadBufferExtended(relation, DATA_FORKNUM, blockNumber, RBM_NORMAL, NULL);

		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buffer);
		pageHeader = (PageHeader ) page;

		START_CRIT_SECTION();

		if (blockNumber == P_NEW)
		{
			PageInit(page, BLCKSZ, 0);
			blockNumber = BufferGetBlockNumber(buffer);
		}

		Assert(pageHeader->pd_lower > 0);

		pageData = PageGetContents(page);


		remainingCapacity = pageHeader->pd_upper - pageHeader->pd_lower;
		copySize = dataLength - dataOffset;

		if (remainingCapacity < copySize)
		{
			copySize = remainingCapacity;
		}

		pageOffset = (pageHeader->pd_lower) - SizeOfPageHeaderData;

		memcpy(pageData + pageOffset, (char *) data + dataOffset, copySize);

		dataOffset += copySize;

		pageHeader->pd_lower += copySize;
		if (pageHeader->pd_lower >= pageHeader->pd_upper)
		{
			pageHeader->pd_flags |= PD_PAGE_FULL;
		}

		MarkBufferDirty(buffer);

		if (writeState->logging)
		{
			LOG_NEWPAGE_BUFFER(buffer);
		}

		END_CRIT_SECTION();

		UnlockReleaseBuffer(buffer);

		/* store last written block number to write state */
		writeState->activeBlockNumber = blockNumber;

		/*
		 * Incrementing blockNumber is necessary for next iteration of the loop
		 * when data to be copied can not fit into active block.
		 */
		blockNumber++;
	}
}


/*
 * CopyStringInfo creates a deep copy of given source string allocating only needed
 * amount of memory.
 */
static StringInfo
CopyStringInfo(StringInfo sourceString)
{
	StringInfo targetString = palloc0(sizeof(StringInfoData));

	if (sourceString->len > 0)
	{
		targetString->data = palloc0(sourceString->len);
		targetString->len = sourceString->len;
		targetString->maxlen = sourceString->len;
		memcpy(targetString->data, sourceString->data, sourceString->len);
	}

	return targetString;
}
