#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sendfile.h>
#include <unistd.h>

// #include "taosdef.h"
// #include "disk.h"
#include "tsdb.h"
#include "tsdbCache.h"
#include "tsdbFile.h"
#include "tsdbMeta.h"
#include "tutil.h"
#include "tskiplist.h"

#define TSDB_DEFAULT_PRECISION TSDB_PRECISION_MILLI  // default precision
#define IS_VALID_PRECISION(precision) (((precision) >= TSDB_PRECISION_MILLI) && ((precision) <= TSDB_PRECISION_NANO))
#define TSDB_MIN_ID 0
#define TSDB_MAX_ID INT_MAX
#define TSDB_MIN_TABLES 10
#define TSDB_MAX_TABLES 100000
#define TSDB_DEFAULT_TABLES 1000
#define TSDB_DEFAULT_DAYS_PER_FILE 10
#define TSDB_MIN_DAYS_PER_FILE 1
#define TSDB_MAX_DAYS_PER_FILE 60
#define TSDB_DEFAULT_MIN_ROW_FBLOCK 100
#define TSDB_MIN_MIN_ROW_FBLOCK 10
#define TSDB_MAX_MIN_ROW_FBLOCK 1000
#define TSDB_DEFAULT_MAX_ROW_FBLOCK 4096
#define TSDB_MIN_MAX_ROW_FBLOCK 200
#define TSDB_MAX_MAX_ROW_FBLOCK 10000
#define TSDB_DEFAULT_KEEP 3650
#define TSDB_MIN_KEEP 1
#define TSDB_MAX_KEEP INT_MAX
#define TSDB_DEFAULT_CACHE_SIZE (16 * 1024 * 1024)  // 16M
#define TSDB_MIN_CACHE_SIZE (4 * 1024 * 1024)       // 4M
#define TSDB_MAX_CACHE_SIZE (1024 * 1024 * 1024)    // 1G

#define TSDB_CFG_FILE_NAME "CONFIG"
#define TSDB_DATA_DIR_NAME "data"
#define TSDB_DEFAULT_FILE_BLOCK_ROW_OPTION 0.7
#define TSDB_MAX_LAST_FILE_SIZE (1024 * 1024 * 10) // 10M

enum { TSDB_REPO_STATE_ACTIVE, TSDB_REPO_STATE_CLOSED, TSDB_REPO_STATE_CONFIGURING };

typedef struct _tsdb_repo {
  char *rootDir;
  // TSDB configuration
  STsdbCfg config;

  // The meter meta handle of this TSDB repository
  STsdbMeta *tsdbMeta;

  // The cache Handle
  STsdbCache *tsdbCache;

  // The TSDB file handle
  STsdbFileH *tsdbFileH;

  // Disk tier handle for multi-tier storage
  void *diskTier;

  pthread_mutex_t mutex;

  int commit;
  pthread_t commitThread;

  // A limiter to monitor the resources used by tsdb
  void *limiter;

  int8_t state;

} STsdbRepo;

static int32_t tsdbCheckAndSetDefaultCfg(STsdbCfg *pCfg);
static int32_t tsdbSetRepoEnv(STsdbRepo *pRepo);
static int32_t tsdbDestroyRepoEnv(STsdbRepo *pRepo);
static int     tsdbOpenMetaFile(char *tsdbDir);
static int32_t tsdbInsertDataToTable(tsdb_repo_t *repo, SSubmitBlk *pBlock);
static int32_t tsdbRestoreCfg(STsdbRepo *pRepo, STsdbCfg *pCfg);
static int32_t tsdbGetDataDirName(STsdbRepo *pRepo, char *fname);
static void *  tsdbCommitData(void *arg);
static int     tsdbCommitToFile(STsdbRepo *pRepo, int fid, SSkipListIterator **iters, SDataCols *pCols);
static int     tsdbHasDataInRange(SSkipListIterator *pIter, TSKEY minKey, TSKEY maxKey);
static int     tsdbHasDataToCommit(SSkipListIterator **iters, int nIters, TSKEY minKey, TSKEY maxKey);

#define TSDB_GET_TABLE_BY_ID(pRepo, sid) (((STSDBRepo *)pRepo)->pTableList)[sid]
#define TSDB_GET_TABLE_BY_NAME(pRepo, name)
#define TSDB_IS_REPO_ACTIVE(pRepo) ((pRepo)->state == TSDB_REPO_STATE_ACTIVE)
#define TSDB_IS_REPO_CLOSED(pRepo) ((pRepo)->state == TSDB_REPO_STATE_CLOSED)

/**
 * Set the default TSDB configuration
 */
void tsdbSetDefaultCfg(STsdbCfg *pCfg) {
  if (pCfg == NULL) return;

  pCfg->precision = -1;
  pCfg->tsdbId = 0;
  pCfg->maxTables = -1;
  pCfg->daysPerFile = -1;
  pCfg->minRowsPerFileBlock = -1;
  pCfg->maxRowsPerFileBlock = -1;
  pCfg->keep = -1;
  pCfg->maxCacheSize = -1;
}

/**
 * Create a configuration for TSDB default
 * @return a pointer to a configuration. the configuration object 
 *         must call tsdbFreeCfg to free memory after usage
 */
STsdbCfg *tsdbCreateDefaultCfg() {
  STsdbCfg *pCfg = (STsdbCfg *)malloc(sizeof(STsdbCfg));
  if (pCfg == NULL) return NULL;

  tsdbSetDefaultCfg(pCfg);

  return pCfg;
}

void tsdbFreeCfg(STsdbCfg *pCfg) {
  if (pCfg != NULL) free(pCfg);
}

/**
 * Create a new TSDB repository
 * @param rootDir the TSDB repository root directory
 * @param pCfg the TSDB repository configuration, upper layer need to free the pointer
 * @param limiter the limitation tracker will implement in the future, make it void now
 *
 * @return a TSDB repository handle on success, NULL for failure
 */
tsdb_repo_t *tsdbCreateRepo(char *rootDir, STsdbCfg *pCfg, void *limiter /* TODO */) {
  if (rootDir == NULL) return NULL;

  if (access(rootDir, F_OK | R_OK | W_OK) == -1) return NULL;

  if (tsdbCheckAndSetDefaultCfg(pCfg) < 0) {
    return NULL;
  }

  STsdbRepo *pRepo = (STsdbRepo *)malloc(sizeof(STsdbRepo));
  if (pRepo == NULL) {
    return NULL;
  }

  pRepo->rootDir = strdup(rootDir);
  pRepo->config = *pCfg;
  pRepo->limiter = limiter;
  pthread_mutex_init(&pRepo->mutex, NULL);

  // Create the environment files and directories
  if (tsdbSetRepoEnv(pRepo) < 0) {
    free(pRepo->rootDir);
    free(pRepo);
    return NULL;
  }

  // Initialize meta
  STsdbMeta *pMeta = tsdbInitMeta(rootDir, pCfg->maxTables);
  if (pMeta == NULL) {
    free(pRepo->rootDir);
    free(pRepo);
    return NULL;
  }
  pRepo->tsdbMeta = pMeta;

  // Initialize cache
  STsdbCache *pCache = tsdbInitCache(pCfg->maxCacheSize, -1, (tsdb_repo_t *)pRepo);
  if (pCache == NULL) {
    free(pRepo->rootDir);
    tsdbFreeMeta(pRepo->tsdbMeta);
    free(pRepo);
    return NULL;
  }
  pRepo->tsdbCache = pCache;

  // Initialize file handle
  char dataDir[128] = "\0";
  tsdbGetDataDirName(pRepo, dataDir);
  pRepo->tsdbFileH =
      tsdbInitFileH(dataDir, pCfg->maxTables);
  if (pRepo->tsdbFileH == NULL) {
    free(pRepo->rootDir);
    tsdbFreeCache(pRepo->tsdbCache);
    tsdbFreeMeta(pRepo->tsdbMeta);
    free(pRepo);
    return NULL;
  }

  pRepo->state = TSDB_REPO_STATE_ACTIVE;

  return (tsdb_repo_t *)pRepo;
}

/**
 * Close and free all resources taken by the repository
 * @param repo the TSDB repository handle. The interface will free the handle too, so upper
 *              layer do NOT need to free the repo handle again.
 *
 * @return 0 for success, -1 for failure and the error number is set
 */
int32_t tsdbDropRepo(tsdb_repo_t *repo) {
  STsdbRepo *pRepo = (STsdbRepo *)repo;

  pRepo->state = TSDB_REPO_STATE_CLOSED;

  // Free the metaHandle
  tsdbFreeMeta(pRepo->tsdbMeta);

  // Free the cache
  tsdbFreeCache(pRepo->tsdbCache);

  // Destroy the repository info
  tsdbDestroyRepoEnv(pRepo);

  free(pRepo->rootDir);
  free(pRepo);

  return 0;
}

/**
 * Open an existing TSDB storage repository
 * @param tsdbDir the existing TSDB root directory
 *
 * @return a TSDB repository handle on success, NULL for failure and the error number is set
 */
tsdb_repo_t *tsdbOpenRepo(char *tsdbDir) {
  if (access(tsdbDir, F_OK | W_OK | R_OK) < 0) {
    return NULL;
  }

  STsdbRepo *pRepo = (STsdbRepo *)malloc(sizeof(STsdbRepo));
  if (pRepo == NULL) {
    return NULL;
  }

  pRepo->rootDir = strdup(tsdbDir);

  tsdbRestoreCfg(pRepo, &(pRepo->config));

  pRepo->tsdbMeta = tsdbInitMeta(tsdbDir, pRepo->config.maxTables);
  if (pRepo->tsdbMeta == NULL) {
    free(pRepo->rootDir);
    free(pRepo);
    return NULL;
  }

  pRepo->tsdbCache = tsdbInitCache(pRepo->config.maxCacheSize, -1, (tsdb_repo_t *)pRepo);
  if (pRepo->tsdbCache == NULL) {
    tsdbFreeMeta(pRepo->tsdbMeta);
    free(pRepo->rootDir);
    free(pRepo);
    return NULL;
  }

  pRepo->state = TSDB_REPO_STATE_ACTIVE;

  return (tsdb_repo_t *)pRepo;
}

static int32_t tsdbFlushCache(STsdbRepo *pRepo) {
  // TODO
  return 0;
}

/**
 * Close a TSDB repository. Only free memory resources, and keep the files.
 * @param repo the opened TSDB repository handle. The interface will free the handle too, so upper
 *              layer do NOT need to free the repo handle again.
 *
 * @return 0 for success, -1 for failure and the error number is set
 */
int32_t tsdbCloseRepo(tsdb_repo_t *repo) {
  STsdbRepo *pRepo = (STsdbRepo *)repo;
  if (pRepo == NULL) return 0;

  pRepo->state = TSDB_REPO_STATE_CLOSED;

  tsdbFlushCache(pRepo);

  tsdbFreeMeta(pRepo->tsdbMeta);

  tsdbFreeCache(pRepo->tsdbCache);

  return 0;
}

/**
 * Change the configuration of a repository
 * @param pCfg the repository configuration, the upper layer should free the pointer
 *
 * @return 0 for success, -1 for failure and the error number is set
 */
int32_t tsdbConfigRepo(tsdb_repo_t *repo, STsdbCfg *pCfg) {
  STsdbRepo *pRepo = (STsdbRepo *)repo;

  pRepo->config = *pCfg;
  // TODO
  return 0;
}

int32_t tsdbTriggerCommit(tsdb_repo_t *repo) {
  STsdbRepo *pRepo = (STsdbRepo *)repo;
  
  tsdbLockRepo(repo);
  if (pRepo->commit) {
    tsdbUnLockRepo(repo);
    return -1;
  }
  pRepo->commit = 1;
  // Loop to move pData to iData
  for (int i = 0; i < pRepo->config.maxTables; i++) {
    STable *pTable = pRepo->tsdbMeta->tables[i];
    if (pTable != NULL && pTable->mem != NULL) {
      pTable->imem = pTable->mem;
      pTable->mem = NULL;
    }
  }
  // TODO: Loop to move mem to imem
  pRepo->tsdbCache->imem = pRepo->tsdbCache->mem;
  pRepo->tsdbCache->mem = NULL;
  pRepo->tsdbCache->curBlock = NULL;

  // TODO: here should set as detached or use join for memory leak
  pthread_create(&(pRepo->commitThread), NULL, tsdbCommitData, (void *)repo);
  tsdbUnLockRepo(repo);

  return 0;
}

int32_t tsdbLockRepo(tsdb_repo_t *repo) {
  STsdbRepo *pRepo = (STsdbRepo *)repo;
  return pthread_mutex_lock(repo);
}

int32_t tsdbUnLockRepo(tsdb_repo_t *repo) {
  STsdbRepo *pRepo = (STsdbRepo *)repo;
  return pthread_mutex_unlock(repo);
}

/**
 * Get the TSDB repository information, including some statistics
 * @param pRepo the TSDB repository handle
 * @param error the error number to set when failure occurs
 *
 * @return a info struct handle on success, NULL for failure and the error number is set. The upper
 *         layers should free the info handle themselves or memory leak will occur
 */
STsdbRepoInfo *tsdbGetStatus(tsdb_repo_t *pRepo) {
  // TODO
  return NULL;
}

int tsdbCreateTable(tsdb_repo_t *repo, STableCfg *pCfg) {
  STsdbRepo *pRepo = (STsdbRepo *)repo;
  return tsdbCreateTableImpl(pRepo->tsdbMeta, pCfg);
}

int tsdbAlterTable(tsdb_repo_t *pRepo, STableCfg *pCfg) {
  // TODO
  return 0;
}

int tsdbDropTable(tsdb_repo_t *repo, STableId tableId) {
  if (repo == NULL) return -1;
  STsdbRepo *pRepo = (STsdbRepo *)repo;

  return tsdbDropTableImpl(pRepo->tsdbMeta, tableId);
}

STableInfo *tsdbGetTableInfo(tsdb_repo_t *pRepo, STableId tableId) {
  // TODO
  return NULL;
}

// TODO: need to return the number of data inserted
int32_t tsdbInsertData(tsdb_repo_t *repo, SSubmitMsg *pMsg) {
  SSubmitMsgIter msgIter;

  tsdbInitSubmitMsgIter(pMsg, &msgIter);
  SSubmitBlk *pBlock;
  while ((pBlock = tsdbGetSubmitMsgNext(&msgIter)) != NULL) {
    if (tsdbInsertDataToTable(repo, pBlock) < 0) {
      return -1;
    }
  }

  return 0;
}

/**
 * Initialize a table configuration
 */
int tsdbInitTableCfg(STableCfg *config, TSDB_TABLE_TYPE type, int64_t uid, int32_t tid) {
  if (config == NULL) return -1;
  if (type != TSDB_NORMAL_TABLE && type != TSDB_CHILD_TABLE) return -1;

  memset((void *)config, 0, sizeof(STableCfg));

  config->type = type;
  config->superUid = TSDB_INVALID_SUPER_TABLE_ID;
  config->tableId.uid = uid;
  config->tableId.tid = tid;
  return 0;
}

/**
 * Set the super table UID of the created table
 */
int tsdbTableSetSuperUid(STableCfg *config, int64_t uid) {
  if (config->type != TSDB_CHILD_TABLE) return -1;
  if (uid == TSDB_INVALID_SUPER_TABLE_ID) return -1;

  config->superUid = uid;
  return 0;
}

/**
 * Set the table schema in the configuration
 * @param config the configuration to set
 * @param pSchema the schema to set
 * @param dup use the schema directly or duplicate one for use
 * 
 * @return 0 for success and -1 for failure
 */
int tsdbTableSetSchema(STableCfg *config, STSchema *pSchema, bool dup) {
  if (dup) {
    config->schema = tdDupSchema(pSchema);
  } else {
    config->schema = pSchema;
  }
  return 0;
}

/**
 * Set the table schema in the configuration
 * @param config the configuration to set
 * @param pSchema the schema to set
 * @param dup use the schema directly or duplicate one for use
 * 
 * @return 0 for success and -1 for failure
 */
int tsdbTableSetTagSchema(STableCfg *config, STSchema *pSchema, bool dup) {
  if (config->type != TSDB_CHILD_TABLE) return -1;

  if (dup) {
    config->tagSchema = tdDupSchema(pSchema);
  } else {
    config->tagSchema = pSchema;
  }
  return 0;
}

int tsdbTableSetTagValue(STableCfg *config, SDataRow row, bool dup) {
  if (config->type != TSDB_CHILD_TABLE) return -1;

  if (dup) {
    config->tagValues = tdDataRowDup(row);
  } else {
    config->tagValues = row;
  }

  return 0;
}

void tsdbClearTableCfg(STableCfg *config) {
  if (config->schema) tdFreeSchema(config->schema);
  if (config->tagSchema) tdFreeSchema(config->tagSchema);
  if (config->tagValues) tdFreeDataRow(config->tagValues);
}

int tsdbInitSubmitBlkIter(SSubmitBlk *pBlock, SSubmitBlkIter *pIter) {
  if (pBlock->len <= 0) return -1;
  pIter->totalLen = pBlock->len;
  pIter->len = 0;
  pIter->row = (SDataRow)(pBlock->data);
  return 0;
}

SDataRow tsdbGetSubmitBlkNext(SSubmitBlkIter *pIter) {
  SDataRow row = pIter->row;
  if (row == NULL) return NULL;

  pIter->len += dataRowLen(row);
  if (pIter->len >= pIter->totalLen) {
    pIter->row = NULL;
  } else {
    pIter->row = (char *)row + dataRowLen(row);
  }

  return row;
}

int tsdbInitSubmitMsgIter(SSubmitMsg *pMsg, SSubmitMsgIter *pIter) {
  if (pMsg == NULL || pIter == NULL) return -1;

  pMsg->length = htonl(pMsg->length);
  pMsg->numOfBlocks = htonl(pMsg->numOfBlocks);
  pMsg->compressed = htonl(pMsg->compressed);
  
  pIter->totalLen = pMsg->length;
  pIter->len = TSDB_SUBMIT_MSG_HEAD_SIZE;
  if (pMsg->length <= TSDB_SUBMIT_MSG_HEAD_SIZE) {
    pIter->pBlock = NULL;
  } else {
    pIter->pBlock = pMsg->blocks;
  }

  return 0;
}

SSubmitBlk *tsdbGetSubmitMsgNext(SSubmitMsgIter *pIter) {
  SSubmitBlk *pBlock = pIter->pBlock;
  if (pBlock == NULL) return NULL;
  
  pBlock->len = htonl(pBlock->len);
  pBlock->numOfRows = htons(pBlock->numOfRows);
  pBlock->uid = htobe64(pBlock->uid);
  pBlock->tid = htonl(pBlock->tid);
  
  pBlock->sversion = htonl(pBlock->sversion);
  pBlock->padding = htonl(pBlock->padding);
  
  pIter->len = pIter->len + sizeof(SSubmitBlk) + pBlock->len;
  if (pIter->len >= pIter->totalLen) {
    pIter->pBlock = NULL;
  } else {
    pIter->pBlock = (SSubmitBlk *)((char *)pBlock + pBlock->len + sizeof(SSubmitBlk));
  }

  return pBlock;
}

STsdbMeta* tsdbGetMeta(tsdb_repo_t* pRepo) {
  STsdbRepo *tsdb = (STsdbRepo *)pRepo;
  return tsdb->tsdbMeta;
}

// Check the configuration and set default options
static int32_t tsdbCheckAndSetDefaultCfg(STsdbCfg *pCfg) {
  // Check precision
  if (pCfg->precision == -1) {
    pCfg->precision = TSDB_DEFAULT_PRECISION;
  } else {
    if (!IS_VALID_PRECISION(pCfg->precision)) return -1;
  }

  // Check tsdbId
  if (pCfg->tsdbId < 0) return -1;

  // Check maxTables
  if (pCfg->maxTables == -1) {
    pCfg->maxTables = TSDB_DEFAULT_TABLES;
  } else {
    if (pCfg->maxTables < TSDB_MIN_TABLES || pCfg->maxTables > TSDB_MAX_TABLES) return -1;
  }

  // Check daysPerFile
  if (pCfg->daysPerFile == -1) {
    pCfg->daysPerFile = TSDB_DEFAULT_DAYS_PER_FILE;
  } else {
    if (pCfg->daysPerFile < TSDB_MIN_DAYS_PER_FILE || pCfg->daysPerFile > TSDB_MAX_DAYS_PER_FILE) return -1;
  }

  // Check minRowsPerFileBlock and maxRowsPerFileBlock
  if (pCfg->minRowsPerFileBlock == -1) {
    pCfg->minRowsPerFileBlock = TSDB_DEFAULT_MIN_ROW_FBLOCK;
  } else {
    if (pCfg->minRowsPerFileBlock < TSDB_MIN_MIN_ROW_FBLOCK || pCfg->minRowsPerFileBlock > TSDB_MAX_MIN_ROW_FBLOCK)
      return -1;
  }

  if (pCfg->maxRowsPerFileBlock == -1) {
    pCfg->maxRowsPerFileBlock = TSDB_DEFAULT_MAX_ROW_FBLOCK;
  } else {
    if (pCfg->maxRowsPerFileBlock < TSDB_MIN_MAX_ROW_FBLOCK || pCfg->maxRowsPerFileBlock > TSDB_MAX_MAX_ROW_FBLOCK)
      return -1;
  }

  if (pCfg->minRowsPerFileBlock > pCfg->maxRowsPerFileBlock) return -1;

  // Check keep
  if (pCfg->keep == -1) {
    pCfg->keep = TSDB_DEFAULT_KEEP;
  } else {
    if (pCfg->keep < TSDB_MIN_KEEP || pCfg->keep > TSDB_MAX_KEEP) return -1;
  }

  // Check maxCacheSize
  if (pCfg->maxCacheSize == -1) {
    pCfg->maxCacheSize = TSDB_DEFAULT_CACHE_SIZE;
  } else {
    if (pCfg->maxCacheSize < TSDB_MIN_CACHE_SIZE || pCfg->maxCacheSize > TSDB_MAX_CACHE_SIZE) return -1;
  }

  return 0;
}

static int32_t tsdbGetCfgFname(STsdbRepo *pRepo, char *fname) {
  if (pRepo == NULL) return -1;
  sprintf(fname, "%s/%s", pRepo->rootDir, TSDB_CFG_FILE_NAME);
  return 0;
}

static int32_t tsdbSaveConfig(STsdbRepo *pRepo) {
  char fname[128] = "\0"; // TODO: get rid of the literal 128

  if (tsdbGetCfgFname(pRepo, fname) < 0) return -1;

  int fd = open(fname, O_WRONLY | O_CREAT, 0755);
  if (fd < 0) {
    return -1;
  }

  if (write(fd, (void *)(&(pRepo->config)), sizeof(STsdbCfg)) < 0) {
    return -1;
  }

  close(fd);
  return 0;
}

static int32_t tsdbRestoreCfg(STsdbRepo *pRepo, STsdbCfg *pCfg) {
  char fname[128] = "\0";

  if (tsdbGetCfgFname(pRepo, fname) < 0) return -1;

  int fd = open(fname, O_RDONLY);
  if (fd < 0) {
    return -1;
  }

  if (read(fd, (void *)pCfg, sizeof(STsdbCfg)) < sizeof(STsdbCfg)) {
    close(fd);
    return -1;
  }

  close(fd);

  return 0;
}

static int32_t tsdbGetDataDirName(STsdbRepo *pRepo, char *fname) {
  if (pRepo == NULL || pRepo->rootDir == NULL) return -1;
  sprintf(fname, "%s/%s", pRepo->rootDir, TSDB_DATA_DIR_NAME);
  return 0;
}

static int32_t tsdbSetRepoEnv(STsdbRepo *pRepo) {
  if (tsdbSaveConfig(pRepo) < 0) return -1;

  char dirName[128] = "\0";
  if (tsdbGetDataDirName(pRepo, dirName) < 0) return -1;

  if (mkdir(dirName, 0755) < 0) {
    return -1;
  }

  return 0;
}

static int32_t tsdbDestroyRepoEnv(STsdbRepo *pRepo) {
  char fname[128];
  if (pRepo == NULL) return 0;
  char *dirName = calloc(1, strlen(pRepo->rootDir) + strlen("tsdb") + 2);
  if (dirName == NULL) {
    return -1;
  }

  sprintf(dirName, "%s/%s", pRepo->rootDir, "tsdb");

  DIR *dir = opendir(dirName);
  if (dir == NULL) return -1;

  struct dirent *dp;
  while ((dp = readdir(dir)) != NULL) {
    if ((strcmp(dp->d_name, ".") == 0) || (strcmp(dp->d_name, "..") == 0)) continue;
    sprintf(fname, "%s/%s", pRepo->rootDir, dp->d_name);
    remove(fname);
  }

  closedir(dir);

  rmdir(dirName);

  return 0;
}

static int tsdbOpenMetaFile(char *tsdbDir) {
  // TODO
  return 0;
}

static int32_t tdInsertRowToTable(STsdbRepo *pRepo, SDataRow row, STable *pTable) {
  // TODO
  int32_t level = 0;
  int32_t headSize = 0;

  if (pTable->mem == NULL) {
    pTable->mem = (SMemTable *)calloc(1, sizeof(SMemTable));
    if (pTable->mem == NULL) return -1;
    pTable->mem->pData = tSkipListCreate(5, TSDB_DATA_TYPE_TIMESTAMP, TYPE_BYTES[TSDB_DATA_TYPE_TIMESTAMP], 0, 0, getTupleKey);
    pTable->mem->keyFirst = INT64_MAX;
    pTable->mem->keyLast = 0;
  }

  tSkipListRandNodeInfo(pTable->mem->pData, &level, &headSize);

  TSKEY key = dataRowKey(row);
  // printf("insert:%lld, size:%d\n", key, pTable->mem->numOfPoints);
  
  // Copy row into the memory
  SSkipListNode *pNode = tsdbAllocFromCache(pRepo->tsdbCache, headSize + dataRowLen(row), key);
  if (pNode == NULL) {
    // TODO: deal with allocate failure
  }

  pNode->level = level;
  dataRowCpy(SL_GET_NODE_DATA(pNode), row);

  // Insert the skiplist node into the data
  if (pTable->mem == NULL) {
    pTable->mem = (SMemTable *)calloc(1, sizeof(SMemTable));
    if (pTable->mem == NULL) return -1;
    pTable->mem->pData = tSkipListCreate(5, TSDB_DATA_TYPE_TIMESTAMP, TYPE_BYTES[TSDB_DATA_TYPE_TIMESTAMP], 0, 0, getTupleKey);
    pTable->mem->keyFirst = INT64_MAX;
    pTable->mem->keyLast = 0;
  }
  tSkipListPut(pTable->mem->pData, pNode);
  if (key > pTable->mem->keyLast) pTable->mem->keyLast = key;
  if (key < pTable->mem->keyFirst) pTable->mem->keyFirst = key;
  
  pTable->mem->numOfPoints = tSkipListGetSize(pTable->mem->pData);
//  pTable->mem->numOfPoints++;

  return 0;
}

static int32_t tsdbInsertDataToTable(tsdb_repo_t *repo, SSubmitBlk *pBlock) {
  STsdbRepo *pRepo = (STsdbRepo *)repo;

  STableId tableId = {.uid = pBlock->uid, .tid = pBlock->tid};
  STable *pTable = tsdbIsValidTableToInsert(pRepo->tsdbMeta, tableId);
  if (pTable == NULL) return -1;

  SSubmitBlkIter blkIter;
  SDataRow row;

  tsdbInitSubmitBlkIter(pBlock, &blkIter);
  while ((row = tsdbGetSubmitBlkNext(&blkIter)) != NULL) {
    if (tdInsertRowToTable(pRepo, row, pTable) < 0) {
      return -1;
    }
  }

  return 0;
}

static int tsdbReadRowsFromCache(SSkipListIterator *pIter, TSKEY maxKey, int maxRowsToRead, SDataCols *pCols) {
  int numOfRows = 0;

  do {
    SSkipListNode *node = tSkipListIterGet(pIter);
    if (node == NULL) break;

    SDataRow row = SL_GET_NODE_DATA(node);
    if (dataRowKey(row) > maxKey) break;

    tdAppendDataRowToDataCol(row, pCols);

    numOfRows++;
    if (numOfRows >= maxRowsToRead) break;
  } while (tSkipListIterNext(pIter));

  return numOfRows;
}

static void tsdbDestroyTableIters(SSkipListIterator **iters, int maxTables) {
  if (iters == NULL) return;

  for (int tid = 0; tid < maxTables; tid++) {
    if (iters[tid] == NULL) continue;
    tSkipListDestroyIter(iters[tid]);
  }

  free(iters);
}

static SSkipListIterator **tsdbCreateTableIters(STsdbMeta *pMeta, int maxTables) {
  SSkipListIterator **iters = (SSkipListIterator *)calloc(maxTables, sizeof(SSkipListIterator *));
  if (iters == NULL) return NULL;

  for (int tid = 0; tid < maxTables; tid++) {
    STable *pTable = pMeta->tables[tid];
    if (pTable == NULL || pTable->imem == NULL) continue;

    iters[tid] = tSkipListCreateIter(pTable->imem->pData);
    if (iters[tid] == NULL) {
      tsdbDestroyTableIters(iters, maxTables);
      return NULL;
    }

    if (!tSkipListIterNext(iters[tid])) {
      assert(false);
    }
  }

  return iters;
}

// Commit to file
static void *tsdbCommitData(void *arg) {
  // TODO
  printf("Starting to commit....\n");
  STsdbRepo * pRepo = (STsdbRepo *)arg;
  STsdbMeta * pMeta = pRepo->tsdbMeta;
  STsdbCache *pCache = pRepo->tsdbCache;
  STsdbCfg * pCfg = &(pRepo->config);
  if (pCache->imem == NULL) return NULL;

  // Create the iterator to read from cache
  SSkipListIterator **iters = tsdbCreateTableIters(pMeta, pCfg->maxTables);
  if (iters == NULL) {
    // TODO: deal with the error
    return NULL;
  }

  // Create a data column buffer for commit
  SDataCols *pCols = tdNewDataCols(pMeta->maxRowBytes, pMeta->maxCols, pCfg->maxRowsPerFileBlock);
  if (pCols == NULL) {
    // TODO: deal with the error
    return NULL;
  }

  int sfid = tsdbGetKeyFileId(pCache->imem->keyFirst, pCfg->daysPerFile, pCfg->precision);
  int efid = tsdbGetKeyFileId(pCache->imem->keyLast, pCfg->daysPerFile, pCfg->precision);

  for (int fid = sfid; fid <= efid; fid++) {
    if (tsdbCommitToFile(pRepo, fid, iters, pCols) < 0) {
      // TODO: deal with the error here
      // assert(0);
    }
  }

  tdFreeDataCols(pCols);
  tsdbDestroyTableIters(iters, pCfg->maxTables);


  tsdbLockRepo(arg);
  tdListMove(pCache->imem->list, pCache->pool.memPool);
  free(pCache->imem);
  pCache->imem = NULL;
  pRepo->commit = 0;
  // TODO: free the skiplist
  for (int i = 0; i < pCfg->maxTables; i++) {
    STable *pTable = pMeta->tables[i];
    if (pTable && pTable->imem) { // Here has memory leak
      pTable->imem = NULL;
    }
  }
  tsdbUnLockRepo(arg);

  return NULL;
}

static int tsdbCommitToFile(STsdbRepo *pRepo, int fid, SSkipListIterator **iters, SDataCols *pCols) {
  int isNewLastFile = 0;

  STsdbMeta * pMeta = pRepo->tsdbMeta;
  STsdbFileH *pFileH = pRepo->tsdbFileH;
  STsdbCfg *  pCfg = &pRepo->config;
  SFile       hFile, lFile;
  SFileGroup *pGroup = NULL;
  SCompIdx *  pIndices = NULL;
  SCompInfo * pCompInfo = NULL;
  size_t      compInfoSize = 0;
  SCompBlock  compBlock;
  SCompBlock *pBlock = &compBlock;

  TSKEY minKey = 0, maxKey = 0;
  tsdbGetKeyRangeOfFileId(pCfg->daysPerFile, pCfg->precision, fid, &minKey, &maxKey);

  // Check if there are data to commit to this file
  int hasDataToCommit = tsdbHasDataToCommit(iters, pCfg->maxTables, minKey, maxKey);
  if (!hasDataToCommit) return 0; // No data to commit, just return

  // Create and open files for commit
  tsdbGetDataDirName(pRepo, dataDir);
  if (tsdbCreateFGroup(pFileH, dataDir, fid, pCfg->maxTables) < 0) {/* TODO */}
  pGroup = tsdbOpenFilesForCommit(pFileH, fid);
  if (pGroup == NULL) {/* TODO */}
  tsdbCreateFile(dataDir, fid, ".h", pCfg->maxTables, &hFile, 1, 0);
  if (1 /*pGroup->files[TSDB_FILE_TYPE_LAST].size > TSDB_MAX_LAST_FILE_SIZE*/) {
    // TODO: make it not to write the last file every time
    tsdbCreateFile(dataDir, fid, ".l", pCfg->maxTables, &lFile, 0, 0);
    isNewLastFile = 1;
  }

  // Load the SCompIdx
  pIndices = (SCompIdx *)malloc(sizeof(SCompIdx) *pCfg->maxTables);
  if (pIndices == NULL) {/* TODO*/}
  if (tsdbLoadCompIdx(pGroup, (void *)pIndices, pCfg->maxTables) < 0) {/* TODO */}

// Loop to commit data in each table
  for (int tid = 0; tid < pCfg->maxTables; tid++) {
    STable *pTable = pMeta->tables[tid];
    SSkipListIterator *pIter = iters[tid];
    SCompIdx *pIdx = &pIndices[tid];

    if (!tsdbHasDataInRange(pIter, minKey, maxKey)) {
      // This table does not have data in this range, just copy its head part and last
      // part (if neccessary) to new file
      if (pIdx->len != 0) { // has old data
        if (isNewLastFile && pIdx->hasLast) {
          // Need to load SCompBlock part and copy to new file
          if ((pCompInfo = (SCompInfo *)realloc((void *)pCompInfo, pIdx->len)) == NULL) {/* TODO */}
          if (tsdbLoadCompBlocks(pGroup, pIdx, (void *)pCompInfo) < 0) {/* TODO */}

          // Copy the last block from old last file to new file
          // tsdbCopyBlockData()
        } else { 
          pIdx->offset = lseek(hFile.fd, 0, SEEK_CUR);
          sendfile(pGroup->files[TSDB_FILE_TYPE_HEAD].fd, hFile.fd, NULL, pIdx->len);
          hFile.size += pIdx->len;
        }
      }
      continue;
    }

    // while () {

    // }
  }

  // for (int tid = 0; tid < pCfg->maxTables; tid++) {
  //   STable *           pTable = pMeta->tables[tid];
  //   SSkipListIterator *pIter = iters[tid];
  //   int isLoadCompBlocks = 0;
  //   char dataDir[128] = "\0";

  //   if (pIter == NULL) {
  //     if (hasDataToCommit && isNewLastFile())
  //     continue;
  //   }
  //   tdInitDataCols(pCols, pTable->schema);

  //   int numOfWrites = 0;
  //   int maxRowsToRead = pCfg->maxRowsPerFileBlock * 4 / 5; // We keep 20% of space for merge purpose
  //   // Loop to read columns from cache
  //   while (tsdbReadRowsFromCache(pIter, maxKey, maxRowsToRead, pCols)) {
  //     if (!hasDataToCommit) {
  //       // There are data to commit to this fileId, we need to create/open it for read/write.
  //       // At the meantime, we set the flag to prevent further create/open operations
  //       tsdbGetDataDirName(pRepo, dataDir);

  //       if (tsdbCreateFGroup(pFileH, dataDir, fid, pCfg->maxTables) < 0) {
  //         // TODO: deal with the ERROR here
  //       }
  //       // Open files for commit
  //       pGroup = tsdbOpenFilesForCommit(pFileH, fid);
  //       if (pGroup == NULL) {
  //         // TODO: deal with the ERROR here
  //       }
  //       // TODO: open .h file and if neccessary, open .l file
  //       tsdbCreateFile(dataDir, fid, ".h", pCfg->maxTables, &tFile, 1, 0);
  //       if (1 /*pGroup->files[TSDB_FILE_TYPE_LAST].size > TSDB_MAX_LAST_FILE_SIZE*/) {
  //         // TODO: make it not to write the last file every time
  //         tsdbCreateFile(dataDir, fid, ".l", pCfg->maxTables, &lFile, 0, 0);
  //         isNewLastFile = 1;
  //       }

  //       // load the SCompIdx part
  //       pIndices = (SCompIdx *)malloc(sizeof(SCompIdx) * pCfg->maxTables);
  //       if (pIndices == NULL) { // TODO: deal with the ERROR
  //       }
  //       if (tsdbLoadCompIdx(pGroup, (void *)pIndices, pCfg->maxTables) < 0) { // TODO: deal with the ERROR here
  //       }

  //       // sendfile those not need to changed table content
  //       lseek(pGroup->files[TSDB_FILE_TYPE_HEAD].fd, TSDB_FILE_HEAD_SIZE + sizeof(SCompIdx) * pCfg->maxTables,
  //             SEEK_SET);
  //       lseek(tFile.fd, TSDB_FILE_HEAD_SIZE + sizeof(SCompIdx) * pCfg->maxTables, SEEK_SET);
  //       for (int ttid = 0; ttid < tid; ttid++) {
  //         SCompIdx * tIdx= &pIndices[ttid];
  //         if (tIdx->len <= 0) continue;
  //         if (isNewLastFile && tIdx->hasLast) {
  //           // TODO: Need to load the SCompBlock part and copy to new last file
  //           pCompInfo = (SCompInfo *)realloc((void *)pCompInfo, tIdx->len);
  //           if (pCompInfo == NULL) { /* TODO */}
  //           if (tsdbLoadCompBlocks(pGroup, tIdx, (void *)pCompInfo) < 0) {/* TODO */}
  //           SCompBlock *pLastBlock = TSDB_COMPBLOCK_AT(pCompInfo, tIdx->numOfSuperBlocks - 1);
  //           int numOfSubBlocks = pLastBlock->numOfSubBlocks;
  //           assert(pLastBlock->last);
  //           if (tsdbCopyCompBlockToFile(&pGroup->files[TSDB_FILE_TYPE_LAST], &lFile, pCompInfo, tIdx->numOfSuperBlocks, 1 /* isOutLast*/) < 0) {/* TODO */}
  //           {
  //             if (numOfSubBlocks > 1) tIdx->len -= (sizeof(SCompBlock) * numOfSubBlocks);
  //             tIdx->checksum = 0;
  //           }
  //           write(tFile.fd, (void *)pCompInfo, tIdx->len);
  //           tFile.size += tIdx->len;
  //         } else {
  //           sendfile(pGroup->files[TSDB_FILE_TYPE_HEAD].fd, tFile.fd, NULL, tIdx->len);
  //           tFile.size += tIdx->len;
  //         }
  //       }

  //       hasDataToCommit = 1;
  //     }

  //     SCompIdx *pIdx = &pIndices[tid];

  //     /* The first time to write to the table, need to decide
  //      * if it is neccessary to load the SComplock part. If it
  //      * is needed, just load it, or, just use sendfile and
  //      * append it.
  //      */
  //     if (numOfWrites == 0 && pIdx->offset > 0) {
  //       if (dataColsKeyFirst(pCols) <= pIdx->maxKey || pIdx->hasLast) {
  //         pCompInfo = (SCompInfo *)realloc((void *)pCompInfo, pIdx->len);
  //         if (tsdbLoadCompBlocks(pGroup, pIdx, (void *)pCompInfo) < 0) { 
  //           // TODO: deal with the ERROR here
  //         }
  //         if (pCompInfo->uid == pTable->tableId.uid) isLoadCompBlocks = 1;
  //       } else {
  //         // TODO: sendfile the prefix part
  //       }
  //     }

  //     int numOfPointsWritten = tsdbWriteBlockToFile(pGroup, pCompInfo, pIdx, isLoadCompBlocks, pBlock, pCols);
  //     if (numOfPointsWritten < 0) {
  //       // TODO: deal with the ERROR here
  //     }

  //     // pCompInfo = tsdbMergeBlock(pCompInfo, pBlock);


  //     // if (1 /* the SCompBlock part is not loaded*/) {
  //     //   // Append to .data file generate a SCompBlock and record it
  //     // } else {
  //     // }

  //     // // TODO: need to reset the pCols

  //     numOfWrites++;
  //   }

  //   if (pCols->numOfPoints > 0) { 
  //     // TODO: still has data to commit, commit it
  //   }

  //   if (1/* SCompBlock part is loaded, write it to .head file*/) {
  //     // TODO
  //   } else {
  //     // TODO: use sendfile send the old part and append the newly added part
  //   }
  // }

  // Write the SCompIdx part

  // Close all files and return 
  if (hasDataToCommit) {
    // TODO
  }

  if (pIndices) free(pIndices);
  if (pCompInfo) free(pCompInfo);

  return 0;
}

static int tsdbHasDataInRange(SSkipListIterator *pIter, TSKEY minKey, TSKEY maxKey) {
  if (pIter == NULL) return 0;

  SSkipListNode *node = tSkipListIterGet(pIter);
  if (node == NULL) return 0;

  SDataRow row = SL_GET_NODE_DATA(node);
  if (dataRowKey(row) >= minKey && dataRowKey(row) <= maxKey) return 1;

  return 0;
}

static int tsdbHasDataToCommit(SSkipListIterator **iters, int nIters, TSKEY minKey, TSKEY maxKey) {
  for (int i = 0; i < nIters; i++) {
    SSkipListIterator *pIter = iters[i];
    if (tsdbHasDataInRange(pIter, minKey, maxKey)) return 1;
  }
  return 0;
}