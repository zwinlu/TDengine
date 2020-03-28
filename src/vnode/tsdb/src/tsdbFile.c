/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "tsdbFile.h"

#define TSDB_FILE_DELIMITER 0xF00AFA0F

const char *tsdbFileSuffix[] = {
    ".head",  // TSDB_FILE_TYPE_HEAD
    ".data",  // TSDB_FILE_TYPE_DATA
    ".last"   // TSDB_FILE_TYPE_LAST
};

static int compFGroupKey(const void *key, const void *fgroup);
static int compFGroup(const void *arg1, const void *arg2);
static int tsdbGetFileName(char *dataDir, int fileId, char *suffix, char *fname);
static int tsdbWriteFileHead(SFile *pFile);
static int tsdbWriteHeadFileIdx(SFile *pFile, int maxTables);
static SFileGroup *tsdbSearchFGroup(STsdbFileH *pFileH, int fid);

STsdbFileH *tsdbInitFileH(char *dataDir, int maxFiles) {
  STsdbFileH *pFileH = (STsdbFileH *)calloc(1, sizeof(STsdbFileH) + sizeof(SFileGroup) * maxFiles);
  if (pFileH == NULL) {  // TODO: deal with ERROR here
    return NULL;
  }

  pFileH->maxFGroups = maxFiles;

  DIR *dir = opendir(dataDir);
  if (dir == NULL) {
    free(pFileH);
    return NULL;
  }

  struct dirent *dp;
  while ((dp = readdir(dir)) != NULL) {
    if (strncmp(dp->d_name, ".", 1) == 0 || strncmp(dp->d_name, "..", 1) == 0) continue;
    // TODO
  }

  return pFileH;
}

void tsdbCloseFileH(STsdbFileH *pFileH) { free(pFileH); }

int tsdbCreateFGroup(STsdbFileH *pFileH, char *dataDir, int fid, int maxTables) {
  if (pFileH->numOfFGroups >= pFileH->maxFGroups) return -1;

  SFileGroup  fGroup;
  SFileGroup *pFGroup = &fGroup;
  if (tsdbSearchFGroup(pFileH, fid) == NULL) { // if not exists, create one
    pFGroup->fileId = fid;
    for (int type = TSDB_FILE_TYPE_HEAD; type < TSDB_FILE_TYPE_MAX; type++) {
      if (tsdbCreateFile(dataDir, fid, tsdbFileSuffix[type], maxTables, &(pFGroup->files[type]), type == TSDB_FILE_TYPE_HEAD ? 1 : 0, 1) < 0) {
        // TODO: deal with the ERROR here, remove those creaed file
        return -1;
      }
    }

    pFileH->fGroup[pFileH->numOfFGroups++] = fGroup;
    qsort((void *)(pFileH->fGroup), pFileH->numOfFGroups, sizeof(SFileGroup), compFGroup);
  }
  return 0;
}

int tsdbRemoveFileGroup(STsdbFileH *pFileH, int fid) {
  SFileGroup *pGroup =
      bsearch((void *)&fid, (void *)(pFileH->fGroup), pFileH->numOfFGroups, sizeof(SFileGroup), compFGroupKey);
  if (pGroup == NULL) return -1;

  // Remove from disk
  for (int type = TSDB_FILE_TYPE_HEAD; type < TSDB_FILE_TYPE_MAX; type++) {
    remove(pGroup->files[type].fname);
  }

  // Adjust the memory
  int filesBehind = pFileH->numOfFGroups - (((char *)pGroup - (char *)(pFileH->fGroup)) / sizeof(SFileGroup) + 1);
  if (filesBehind > 0) {
    memmove((void *)pGroup, (void *)((char *)pGroup + sizeof(SFileGroup)), sizeof(SFileGroup) * filesBehind);
  }
  pFileH->numOfFGroups--;

  return 0;
}

int tsdbLoadDataBlock(SFile *pFile, SCompBlock *pStartBlock, int numOfBlocks, SDataCols *pCols, SCompData *pCompData) {
  SCompBlock *pBlock = pStartBlock;
  for (int i = 0; i < numOfBlocks; i++) {
    if (tsdbLoadCompCols(pFile, pBlock, (void *)pCompData) < 0) return -1;
    for (int iCol = 0; iCol < pBlock->numOfCols; iCol++) {
      SCompCol *pCompCol = &(pCompData->cols[iCol]);
      pCols->numOfPoints += pBlock->numOfPoints;
      int k = 0;
      for (; k < pCols->numOfCols; k++) {
        if (pCompCol->colId == pCols->cols[k].colId) break;
      }

      if (tsdbLoadColData(pFile, pCompCol, pBlock->offset,
                          (void *)((char *)(pCols->cols[k].pData) + pCols->cols[k].len)) < 0)
        return -1;
    }
    pStartBlock++;
  }
  return 0;
}

int tsdbCopyBlockDataInFile(SFile *pOutFile, SFile *pInFile, SCompInfo *pCompInfo, int idx, int isLast, SDataCols *pCols) {
  SCompBlock *pSuperBlock = TSDB_COMPBLOCK_AT(pCompInfo, idx);
  SCompBlock *pStartBlock = NULL;
  SCompBlock *pBlock = NULL;
  int         numOfBlocks = pSuperBlock->numOfSubBlocks;

  if (numOfBlocks == 1)
    pStartBlock = pSuperBlock;
  else
    pStartBlock = TSDB_COMPBLOCK_AT(pCompInfo, pSuperBlock->offset);

  int maxNumOfCols = 0;
  pBlock = pStartBlock;
  for (int i = 0; i < numOfBlocks; i++) {
    if (pBlock->numOfCols > maxNumOfCols) maxNumOfCols = pBlock->numOfCols;
    pBlock++;
  }

  SCompData *pCompData = (SCompData *)malloc(sizeof(SCompData) + sizeof(SCompCol) * maxNumOfCols);
  if (pCompData == NULL) return -1;

  // Load data from the block
  if (tsdbLoadDataBlock(pOutFile, pStartBlock, numOfBlocks, pCols, pCompData));

  // Write data block to the file
  {
    // TODO
  }


  if (pCompData) free(pCompData);
  return 0;
}

int tsdbLoadCompIdx(SFileGroup *pGroup, void *buf, int maxTables) {
  SFile *pFile = &(pGroup->files[TSDB_FILE_TYPE_HEAD]);
  if (lseek(pFile->fd, TSDB_FILE_HEAD_SIZE, SEEK_SET) < 0) return -1;

  if (read(pFile->fd, buf, sizeof(SCompIdx) * maxTables) < 0) return -1;
  // TODO: need to check the correctness
  return 0;
}

int tsdbLoadCompBlocks(SFileGroup *pGroup, SCompIdx *pIdx, void *buf) {
  SFile *pFile = &(pGroup->files[TSDB_FILE_TYPE_HEAD]);

  if (lseek(pFile->fd, pIdx->offset, SEEK_SET) < 0) return -1;

  if (read(pFile->fd, buf, pIdx->len) < 0) return -1;

  // TODO: need to check the correctness

  return 0;
}

int tsdbLoadCompCols(SFile *pFile, SCompBlock *pBlock, void *buf) {
  // assert(pBlock->numOfSubBlocks == 0 || pBlock->numOfSubBlocks == 1);

  if (lseek(pFile->fd, pBlock->offset, SEEK_SET) < 0) return -1;
  size_t size = sizeof(SCompData) + sizeof(SCompCol) * pBlock->numOfCols;
  if (read(pFile->fd, buf, size) < 0) return -1;

  return 0;
}

int tsdbLoadColData(SFile *pFile, SCompCol *pCol, int64_t blockBaseOffset, void *buf) {
  if (lseek(pFile->fd, blockBaseOffset + pCol->offset, SEEK_SET) < 0) return -1;
  if (read(pFile->fd, buf, pCol->len) < 0) return -1;
  return 0;
}

static int tsdbWriteBlockToFileImpl(SFile *     pFile,              // File to write
                                    SDataCols * pCols,              // Data column buffer
                                    int         numOfPointsToWrie,  // Number of points to write to the file
                                    SCompBlock *pBlock              // SCompBlock to hold block information to return
                                    ) {
  // pBlock->last = 0;
  // pBlock->offset = lseek(pFile->fd, 0, SEEK_END);
  // // pBlock->algorithm = ;
  // pBlock->numOfPoints = pCols->numOfPoints;
  // // pBlock->sversion = ;
  // // pBlock->len = ;
  // pBlock->numOfSubBlocks = 1;
  // pBlock->keyFirst = dataColsKeyFirst(pCols);
  // pBlock->keyLast = dataColsKeyLast(pCols);
  // for (int i = 0; i < pCols->numOfCols; i++) {
  //   // TODO: if all col value is NULL, do not save it
  //   pBlock->numOfCols++;
  //   pCompData->numOfCols++;
  //   SCompCol *pCompCol = pCompData->cols + i;
  //   pCompCol->colId = pCols->cols[i].colId;
  //   pCompCol->type = pCols->cols[i].type;

  //   // pCompCol->len = ;
  //   // pCompCol->offset = ;
  // }

  return 0;
}

int tsdbWriteBlockToFile(SFileGroup *pGroup, SCompInfo *pCompInfo, SCompIdx *pIdx, int isMerge, SCompBlock *pBlock, SDataCols *pCols) {
  memset((void *)pBlock, 0, sizeof(SCompBlock));
  SFile *pFile = NULL;
  SCompData *pCompData = (SCompData *)malloc(sizeof(SCompData) + sizeof(SCompCol) * pCols->numOfCols);
  if (pCompData == NULL) return -1;
  pCompData->delimiter = TSDB_FILE_DELIMITER;
  // pCompData->uid = ;

  if (isMerge) {
    TSKEY keyFirst = dataColsKeyFirst(pCols);
    // 1. Binary search the block the data can merged into

    if (1/* the data should only merged into last file */) {
    } else {
    }
  } else {
    // Write directly to the file without merge
    if (1/*pCols->numOfPoints < pCfg->minRowsPerFileBlock*/) {
      // TODO: write the data to the last file
    } else {
      // TODO: wirte the data to the data file
    }
  }

  // TODO: need to update pIdx

  if (pCompData) free(pCompData);
  return 0;
}

static int compFGroupKey(const void *key, const void *fgroup) {
  int         fid = *(int *)key;
  SFileGroup *pFGroup = (SFileGroup *)fgroup;
  return (fid - pFGroup->fileId);
}

static int compFGroup(const void *arg1, const void *arg2) {
  return ((SFileGroup *)arg1)->fileId - ((SFileGroup *)arg2)->fileId;
}

static int tsdbWriteFileHead(SFile *pFile) {
  char head[TSDB_FILE_HEAD_SIZE] = "\0";

  pFile->size += TSDB_FILE_HEAD_SIZE;

  // TODO: write version and File statistic to the head
  lseek(pFile->fd, 0, SEEK_SET);
  if (write(pFile->fd, head, TSDB_FILE_HEAD_SIZE) < 0) return -1;

  return 0;
}

static int tsdbWriteHeadFileIdx(SFile *pFile, int maxTables) {
  int   size = sizeof(SCompIdx) * maxTables;
  void *buf = calloc(1, size);
  if (buf == NULL) return -1;

  if (lseek(pFile->fd, TSDB_FILE_HEAD_SIZE, SEEK_SET) < 0) {
    free(buf);
    return -1;
  }

  if (write(pFile->fd, buf, size) < 0) {
    free(buf);
    return -1;
  }

  pFile->size += size;

  free(buf);
  return 0;
}

static int tsdbGetFileName(char *dataDir, int fileId, char *suffix, char *fname) {
  if (dataDir == NULL || fname == NULL) return -1;

  sprintf(fname, "%s/f%d%s", dataDir, fileId, suffix);

  return 0;
}

int tsdbOpenFile(SFile *pFile, int oflag) { // TODO: change the function
  if (TSDB_IS_FILE_OPENED(pFile)) return -1;

  pFile->fd = open(pFile->fname, oflag, 0755);
  if (pFile->fd < 0) return -1;

  return 0;
}

SFileGroup * tsdbOpenFilesForCommit(STsdbFileH *pFileH, int fid) {
  SFileGroup *pGroup = tsdbSearchFGroup(pFileH, fid);
  if (pGroup == NULL) return NULL;

  for (int type = TSDB_FILE_TYPE_HEAD; type < TSDB_FILE_TYPE_MAX; type++) {
    tsdbOpenFile(&(pGroup->files[type]), O_RDWR);
  }
  return pGroup;
}

static int tsdbCloseFile(SFile *pFile) {
  if (!TSDB_IS_FILE_OPENED(pFile)) return -1;
  int ret = close(pFile->fd);
  pFile->fd = -1;

  return ret;
}

int tsdbCreateFile(char *dataDir, int fileId, char *suffix, int maxTables, SFile *pFile, int writeHeader, int toClose) {
  memset((void *)pFile, 0, sizeof(SFile));
  pFile->fd = -1;

  tsdbGetFileName(dataDir, fileId, suffix, pFile->fname);
  
  if (access(pFile->fname, F_OK) == 0) {
    // File already exists
    return -1;
  }

  if (tsdbOpenFile(pFile, O_WRONLY | O_CREAT) < 0) {
    // TODO: deal with the ERROR here
    return -1;
  }

  if (writeHeader) {
    if (tsdbWriteHeadFileIdx(pFile, maxTables) < 0) {
      tsdbCloseFile(pFile);
      return -1;
    }
  }

  if (tsdbWriteFileHead(pFile) < 0) {
    tsdbCloseFile(pFile);
    return -1;
  }

  if (toClose) tsdbCloseFile(pFile);

  return 0;
}

void tsdbGetKeyRangeOfFileId(int32_t daysPerFile, int8_t precision, int32_t fileId, TSKEY *minKey,
                                    TSKEY *maxKey) {
  *minKey = fileId * daysPerFile * tsMsPerDay[precision];
  *maxKey = *minKey + daysPerFile * tsMsPerDay[precision] - 1;
}

static SFileGroup *tsdbSearchFGroup(STsdbFileH *pFileH, int fid) {
  if (pFileH->numOfFGroups == 0 || fid < pFileH->fGroup[0].fileId || fid > pFileH->fGroup[pFileH->numOfFGroups - 1].fileId)
    return NULL;
  void *ptr = bsearch((void *)&fid, (void *)(pFileH->fGroup), pFileH->numOfFGroups, sizeof(SFileGroup), compFGroupKey);
  if (ptr == NULL) return NULL;
  return (SFileGroup *)ptr;
}