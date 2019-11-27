/*
 * Tencent is pleased to support the open source community by making
 * MMKV available.
 *
 * Copyright (C) 2018 THL A29 Limited, a Tencent company.
 * All rights reserved.
 *
 * Licensed under the BSD 3-Clause License (the "License"); you may not use
 * this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *       https://opensource.org/licenses/BSD-3-Clause
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "MMKV.h"
#include "CodedInputData.h"
#include "CodedOutputData.h"
#include "InterProcessLock.h"
#include "MMBuffer.h"
#include "MMKVLog.h"
#include "MiniPBCoder.h"
#include "MmapedFile.h"
#include "PBUtility.h"
#include "ScopedLock.hpp"
#include "aes/AESCrypt.h"
#include "aes/openssl/md5.h"
#include "crc32/Checksum.h"
#include "native-bridge.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace std;

static unordered_map<std::string, MMKV *> *g_instanceDic;
static ThreadLock g_instanceLock;
static std::string g_rootDir;

#define DEFAULT_MMAP_ID "mmkv.default"
#define SPECIAL_CHARACTER_DIRECTORY_NAME "specialCharacter"
constexpr uint32_t Fixed32Size = pbFixed32Size(0);

static string mmapedKVKey(const string &mmapID, string *relativePath = nullptr);
static string
mappedKVPathWithID(const string &mmapID, MMKVMode mode, string *relativePath = nullptr);
static string crcPathWithID(const string &mmapID, MMKVMode mode, string *relativePath = nullptr);
static void mkSpecialCharacterFileDirectory();
static string md5(const string &value);
static string encodeFilePath(const string &mmapID);

enum : bool {
    KeepSequence = false,
    IncreaseSequence = true,
};

MMKV::MMKV(
    const std::string &mmapID, int size, MMKVMode mode, string *cryptKey, string *relativePath)
    : m_mmapID(mmapedKVKey(mmapID, relativePath))
    , m_path(mappedKVPathWithID(m_mmapID, mode, relativePath))
    , m_crcPath(crcPathWithID(m_mmapID, mode, relativePath))
    , m_metaFile(m_crcPath, DEFAULT_MMAP_SIZE, (mode & MMKV_ASHMEM) ? MMAP_ASHMEM : MMAP_FILE)
    , m_crypter(nullptr)
    , m_fileLock(m_metaFile.getFd())
    , m_sharedProcessLock(&m_fileLock, SharedLockType)
    , m_exclusiveProcessLock(&m_fileLock, ExclusiveLockType)
    , m_isInterProcess((mode & MMKV_MULTI_PROCESS) != 0 || (mode & CONTEXT_MODE_MULTI_PROCESS) != 0)
    , m_isAshmem((mode & MMKV_ASHMEM) != 0) {
    m_fd = -1;
    m_ptr = nullptr;
    m_size = 0;
    m_actualSize = 0;
    m_output = nullptr;

    if (m_isAshmem) {
        m_ashmemFile = new MmapedFile(m_mmapID, static_cast<size_t>(size), MMAP_ASHMEM);
        m_fd = m_ashmemFile->getFd();
    } else {
        m_ashmemFile = nullptr;
    }

    if (cryptKey && cryptKey->length() > 0) {
        m_crypter = new AESCrypt((const unsigned char *) cryptKey->data(), cryptKey->length());
    }

    m_needLoadFromFile = true;
    m_hasFullWriteback = false;

    m_crcDigest = 0;

    m_sharedProcessLock.m_enable = m_isInterProcess;
    m_exclusiveProcessLock.m_enable = m_isInterProcess;

    // sensitive zone
    {
        SCOPEDLOCK(m_sharedProcessLock);
        loadFromFile();
    }
}

MMKV::MMKV(const string &mmapID, int ashmemFD, int ashmemMetaFD, string *cryptKey)
    : m_mmapID(mmapID)
    , m_path("")
    , m_crcPath("")
    , m_metaFile(ashmemMetaFD)
    , m_crypter(nullptr)
    , m_fileLock(m_metaFile.getFd())
    , m_sharedProcessLock(&m_fileLock, SharedLockType)
    , m_exclusiveProcessLock(&m_fileLock, ExclusiveLockType)
    , m_isInterProcess(true)
    , m_isAshmem(true) {

    // check mmapID with ashmemID
    {
        auto ashmemID = m_metaFile.getName();
        size_t pos = ashmemID.find_last_of('.');
        if (pos != string::npos) {
            ashmemID.erase(pos, string::npos);
        }
        if (mmapID != ashmemID) {
            MMKVWarning("mmapID[%s] != ashmem[%s]", mmapID.c_str(), ashmemID.c_str());
        }
    }
    m_path = string(ASHMEM_NAME_DEF) + "/" + m_mmapID;
    m_crcPath = string(ASHMEM_NAME_DEF) + "/" + m_metaFile.getName();
    m_fd = ashmemFD;
    m_ptr = nullptr;
    m_size = 0;
    m_actualSize = 0;
    m_output = nullptr;

    if (m_isAshmem) {
        m_ashmemFile = new MmapedFile(m_fd);
    } else {
        m_ashmemFile = nullptr;
    }

    if (cryptKey && cryptKey->length() > 0) {
        m_crypter = new AESCrypt((const unsigned char *) cryptKey->data(), cryptKey->length());
    }

    m_needLoadFromFile = true;
    m_hasFullWriteback = false;

    m_crcDigest = 0;

    m_sharedProcessLock.m_enable = m_isInterProcess;
    m_exclusiveProcessLock.m_enable = m_isInterProcess;

    // sensitive zone
    {
        SCOPEDLOCK(m_sharedProcessLock);
        loadFromFile();
    }
}

MMKV::~MMKV() {
    clearMemoryState();

    if (m_ashmemFile) {
        delete m_ashmemFile;
        m_ashmemFile = nullptr;
    }
    if (m_crypter) {
        delete m_crypter;
        m_crypter = nullptr;
    }
}

MMKV *MMKV::defaultMMKV(MMKVMode mode, string *cryptKey) {
    return mmkvWithID(DEFAULT_MMAP_ID, DEFAULT_MMAP_SIZE, mode, cryptKey);
}

void initialize() {
    g_instanceDic = new unordered_map<std::string, MMKV *>;
    g_instanceLock = ThreadLock();

    //testAESCrypt();

    MMKVInfo("page size:%d", DEFAULT_MMAP_SIZE);
}

void MMKV::initializeMMKV(const std::string &rootDir) {
    static pthread_once_t once_control = PTHREAD_ONCE_INIT;
    pthread_once(&once_control, initialize);

    g_rootDir = rootDir;
    char *path = strdup(g_rootDir.c_str());
    if (path) {
        mkPath(path);
        free(path);
    }

    MMKVInfo("root dir: %s", g_rootDir.c_str());
}

MMKV *MMKV::mmkvWithID(
    const std::string &mmapID, int size, MMKVMode mode, string *cryptKey, string *relativePath) {

    if (mmapID.empty()) {
        return nullptr;
    }
    SCOPEDLOCK(g_instanceLock);

    auto mmapKey = mmapedKVKey(mmapID, relativePath);
    auto itr = g_instanceDic->find(mmapKey);
    if (itr != g_instanceDic->end()) {
        MMKV *kv = itr->second;
        return kv;
    }
    if (relativePath) {
        auto filePath = mappedKVPathWithID(mmapID, mode, relativePath);
        if (!isFileExist(filePath)) {
            if (!createFile(filePath)) {
                return nullptr;
            }
        }
        MMKVInfo("prepare to load %s (id %s) from relativePath %s", mmapID.c_str(), mmapKey.c_str(),
                 relativePath->c_str());
    }
    auto kv = new MMKV(mmapID, size, mode, cryptKey, relativePath);
    (*g_instanceDic)[mmapKey] = kv;
    return kv;
}

MMKV *MMKV::mmkvWithAshmemFD(const string &mmapID, int fd, int metaFD, string *cryptKey) {

    if (fd < 0) {
        return nullptr;
    }
    SCOPEDLOCK(g_instanceLock);

    auto itr = g_instanceDic->find(mmapID);
    if (itr != g_instanceDic->end()) {
        MMKV *kv = itr->second;
        kv->checkReSetCryptKey(fd, metaFD, cryptKey);
        return kv;
    }
    auto kv = new MMKV(mmapID, fd, metaFD, cryptKey);
    (*g_instanceDic)[mmapID] = kv;
    return kv;
}

void MMKV::onExit() {
    SCOPEDLOCK(g_instanceLock);

    for (auto &itr : *g_instanceDic) {
        MMKV *kv = itr.second;
        kv->sync();
        kv->clearMemoryState();
    }
}

const std::string &MMKV::mmapID() {
    return m_mmapID;
}

std::string MMKV::cryptKey() {
    SCOPEDLOCK(m_lock);

    if (m_crypter) {
        char key[AES_KEY_LEN];
        m_crypter->getKey(key);
        return string(key, strnlen(key, AES_KEY_LEN));
    }
    return "";
}

#pragma mark - really dirty work

void decryptBuffer(AESCrypt &crypter, MMBuffer &inputBuffer) {
    size_t length = inputBuffer.length();
    MMBuffer tmp(length);

    auto input = (unsigned char *) inputBuffer.getPtr();
    auto output = (unsigned char *) tmp.getPtr();
    crypter.decrypt(input, output, length);

    inputBuffer = std::move(tmp);
}

void MMKV::loadFromFile() {
    if (m_isAshmem) {
        loadFromAshmem();
        return;
    }

    if (m_metaFile.isFileValid()) {
        m_metaInfo.read(m_metaFile.getMemory());
    }
    if (m_crypter) {
        if (m_metaInfo.m_version >= MMKVVersionRandomIV) {
            m_crypter->reset(m_metaInfo.m_vector, sizeof(m_metaInfo.m_vector));
        }
    }

    m_fd = open(m_path.c_str(), O_RDWR | O_CREAT, S_IRWXU);
    if (m_fd < 0) {
        MMKVError("fail to open:%s, %s", m_path.c_str(), strerror(errno));
    } else {
        m_size = 0;
        struct stat st = {0};
        if (fstat(m_fd, &st) != -1) {
            m_size = static_cast<size_t>(st.st_size);
        }
        // round up to (n * pagesize)
        if (m_size < DEFAULT_MMAP_SIZE || (m_size % DEFAULT_MMAP_SIZE != 0)) {
            size_t oldSize = m_size;
            m_size = ((m_size / DEFAULT_MMAP_SIZE) + 1) * DEFAULT_MMAP_SIZE;
            if (ftruncate(m_fd, m_size) != 0) {
                MMKVError("fail to truncate [%s] to size %zu, %s", m_mmapID.c_str(), m_size,
                          strerror(errno));
                m_size = static_cast<size_t>(st.st_size);
            }
            zeroFillFile(m_fd, oldSize, m_size - oldSize);
        }
        m_ptr = (char *) mmap(nullptr, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
        if (m_ptr == MAP_FAILED) {
            MMKVError("fail to mmap [%s], %s", m_mmapID.c_str(), strerror(errno));
        } else {
            // error checking
            bool loadFromFile = false, needFullWriteback = false;
            checkDataValid(loadFromFile, needFullWriteback);
            MMKVInfo("loading [%s] with %zu actual size, file size %zu, InterProcess %d, meta info "
                     "version:%u",
                     m_mmapID.c_str(), m_actualSize, m_size, m_isInterProcess,
                     m_metaInfo.m_version);
            // loading
            if (loadFromFile && m_actualSize > 0) {
                MMKVInfo("loading [%s] with crc %u sequence %u version %u", m_mmapID.c_str(),
                         m_metaInfo.m_crcDigest, m_metaInfo.m_sequence, m_metaInfo.m_version);
                MMBuffer inputBuffer(m_ptr + Fixed32Size, m_actualSize, MMBufferNoCopy);
                if (m_crypter) {
                    decryptBuffer(*m_crypter, inputBuffer);
                }
                m_dic.clear();
                MiniPBCoder::decodeMap(m_dic, inputBuffer);
                m_output = new CodedOutputData(m_ptr + Fixed32Size, m_size - Fixed32Size);
                m_output->seek(m_actualSize);
                if (needFullWriteback) {
                    fullWriteback();
                }
            } else {
                // file not valid or empty, discard everything
                SCOPEDLOCK(m_exclusiveProcessLock);

                m_output = new CodedOutputData(m_ptr + Fixed32Size, m_size - Fixed32Size);
                if (m_actualSize > 0) {
                    writeActualSize(0, 0, nullptr, IncreaseSequence);
                    sync(true);
                } else {
                    writeActualSize(0, 0, nullptr, KeepSequence);
                }
            }
        }
        MMKVInfo("loaded [%s] with %zu values", m_mmapID.c_str(), m_dic.size());
    }
    if (!isFileValid()) {
        MMKVWarning("[%s] file not valid", m_mmapID.c_str());
    }

    m_needLoadFromFile = false;
}

void MMKV::loadFromAshmem() {
    if (m_metaFile.isFileValid()) {
        m_metaInfo.read(m_metaFile.getMemory());
    }
    if (m_crypter) {
        if (m_metaInfo.m_version >= MMKVVersionRandomIV) {
            m_crypter->reset(m_metaInfo.m_vector, sizeof(m_metaInfo.m_vector));
        }
    }

    if (m_fd < 0 || !m_ashmemFile) {
        MMKVError("ashmem file invalid %s, fd:%d", m_path.c_str(), m_fd);
    } else {
        m_size = m_ashmemFile->getFileSize();
        m_ptr = (char *) m_ashmemFile->getMemory();
        if (m_ptr != MAP_FAILED) {
            m_actualSize = readActualSize();
            MMKVInfo("loading [%s] with %zu size in total, file size is %zu", m_mmapID.c_str(),
                     m_actualSize, m_size);
            bool loaded = false;
            if (m_actualSize > 0) {
                bool loadFromFile = false, needFullWriteback = false;
                // error checking
                checkDataValid(loadFromFile, needFullWriteback);
                if (loadFromFile) {
                    MMKVInfo("loading [%s] with crc %u sequence %u version %u", m_mmapID.c_str(),
                             m_metaInfo.m_crcDigest, m_metaInfo.m_sequence, m_metaInfo.m_version);
                    MMBuffer inputBuffer(m_ptr + Fixed32Size, m_actualSize, MMBufferNoCopy);
                    if (m_crypter) {
                        decryptBuffer(*m_crypter, inputBuffer);
                    }
                    m_dic.clear();
                    MiniPBCoder::decodeMap(m_dic, inputBuffer);
                    m_output = new CodedOutputData(m_ptr + Fixed32Size, m_size - Fixed32Size);
                    m_output->seek(m_actualSize);
                    loaded = true;
                }
            }
            if (!loaded) {
                // file not valid or empty, discard everything
                SCOPEDLOCK(m_exclusiveProcessLock);

                m_output = new CodedOutputData(m_ptr + Fixed32Size, m_size - Fixed32Size);
                if (m_actualSize > 0) {
                    writeActualSize(0, 0, nullptr, IncreaseSequence);
                } else {
                    writeActualSize(0, 0, nullptr, KeepSequence);
                }
            }
            MMKVInfo("loaded [%s] with %zu values", m_mmapID.c_str(), m_dic.size());
        }
    }

    if (!isFileValid()) {
        MMKVWarning("[%s] ashmem not valid", m_mmapID.c_str());
    }

    m_needLoadFromFile = false;
}

// read from last m_position
void MMKV::partialLoadFromFile() {
    m_metaInfo.read(m_metaFile.getMemory());

    size_t oldActualSize = m_actualSize;
    m_actualSize = readActualSize();
    MMKVDebug("loading [%s] with file size %zu, oldActualSize %zu, newActualSize %zu",
              m_mmapID.c_str(), m_size, oldActualSize, m_actualSize);

    if (m_actualSize > 0) {
        if (m_actualSize < m_size && m_actualSize + Fixed32Size <= m_size) {
            if (m_actualSize > oldActualSize) {
                size_t bufferSize = m_actualSize - oldActualSize;
                MMBuffer inputBuffer(m_ptr + Fixed32Size + oldActualSize, bufferSize,
                                     MMBufferNoCopy);
                // incremental update crc digest
                m_crcDigest =
                    (uint32_t) zlib::crc32(m_crcDigest, (const uint8_t *) inputBuffer.getPtr(),
                                           static_cast<uInt>(inputBuffer.length()));
                if (m_crcDigest == m_metaInfo.m_crcDigest) {
                    if (m_crypter) {
                        decryptBuffer(*m_crypter, inputBuffer);
                    }
                    MiniPBCoder::decodeMap(m_dic, inputBuffer, bufferSize);
                    m_output->seek(bufferSize);
                    m_hasFullWriteback = false;

                    MMKVDebug("partial loaded [%s] with %zu values", m_mmapID.c_str(),
                              m_dic.size());
                    return;
                } else {
                    MMKVError("m_crcDigest[%u] != m_metaInfo.m_crcDigest[%u]", m_crcDigest,
                              m_metaInfo.m_crcDigest);
                }
            }
        }
    }
    // something is wrong, do a full load
    clearMemoryState();
    loadFromFile();
}

void MMKV::checkDataValid(bool &loadFromFile, bool &needFullWriteback) {
    // try auto recover from last confirmed location
    constexpr auto offset = pbFixed32Size(0);
    auto checkLastConfirmedInfo = [&] {
        if (m_metaInfo.m_version >= MMKVVersionActualSize) {
            // downgrade & upgrade support
            uint32_t oldStyleActualSize = 0;
            memcpy(&oldStyleActualSize, m_ptr, Fixed32Size);
            if (oldStyleActualSize != m_actualSize) {
                MMKVWarning("oldStyleActualSize %u not equal to meta actual size %lu",
                            oldStyleActualSize, m_actualSize);
                if (checkFileCRCValid(oldStyleActualSize, m_metaInfo.m_crcDigest)) {
                    MMKVInfo("looks like [%s] been downgrade & upgrade again", m_mmapID.c_str());
                    loadFromFile = true;
                    writeActualSize(oldStyleActualSize, m_metaInfo.m_crcDigest, nullptr,
                                    KeepSequence);
                    return;
                }
            }

            auto lastActualSize = m_metaInfo.m_lastConfirmedMetaInfo.lastActualSize;
            if (lastActualSize < m_size && (lastActualSize + offset) <= m_size) {
                auto lastCRCDigest = m_metaInfo.m_lastConfirmedMetaInfo.lastCRCDigest;
                if (checkFileCRCValid(lastActualSize, lastCRCDigest)) {
                    loadFromFile = true;
                    writeActualSize(lastActualSize, lastCRCDigest, nullptr, KeepSequence);
                } else {
                    MMKVError("check [%s] error: lastActualSize %zu, lastActualCRC %zu",
                              m_mmapID.c_str(), lastActualSize, lastCRCDigest);
                }
            } else {
                MMKVError("check [%s] error: lastActualSize %zu, file size is %zu",
                          m_mmapID.c_str(), lastActualSize, m_size);
            }
        }
    };

    m_actualSize = readActualSize();

    if (m_actualSize < m_size && (m_actualSize + offset) <= m_size) {
        if (checkFileCRCValid(m_actualSize, m_metaInfo.m_crcDigest)) {
            loadFromFile = true;
        } else {
            checkLastConfirmedInfo();

            if (!loadFromFile) {
                auto strategic = mmkv::onMMKVCRCCheckFail(m_mmapID);
                if (strategic == OnErrorRecover) {
                    loadFromFile = true;
                    needFullWriteback = true;
                }
                MMKVInfo("recover strategic for [%s] is %d", m_mmapID.c_str(), strategic);
            }
        }
    } else {
        MMKVError("check [%s] error: %zu size in total, file size is %zu", m_mmapID.c_str(),
                  m_actualSize, m_size);

        checkLastConfirmedInfo();

        if (!loadFromFile) {
            auto strategic = mmkv::onMMKVFileLengthError(m_mmapID);
            if (strategic == OnErrorRecover) {
                // make sure we don't over read the file
                m_actualSize = m_size - offset;
                loadFromFile = true;
                needFullWriteback = true;
            }
            MMKVInfo("recover strategic for [%s] is %d", m_mmapID.c_str(), strategic);
        }
    }
}

void MMKV::checkLoadData() {
    if (m_needLoadFromFile) {
        SCOPEDLOCK(m_sharedProcessLock);

        m_needLoadFromFile = false;
        loadFromFile();
        return;
    }
    if (!m_isInterProcess) {
        return;
    }

    if (!m_metaFile.isFileValid()) {
        return;
    }
    // TODO: atomic lock m_metaFile?
    MMKVMetaInfo metaInfo;
    metaInfo.read(m_metaFile.getMemory());
    if (m_metaInfo.m_sequence != metaInfo.m_sequence) {
        MMKVInfo("[%s] oldSeq %u, newSeq %u", m_mmapID.c_str(), m_metaInfo.m_sequence,
                 metaInfo.m_sequence);
        SCOPEDLOCK(m_sharedProcessLock);

        clearMemoryState();
        loadFromFile();
        notifyContentChanged();
    } else if (m_metaInfo.m_crcDigest != metaInfo.m_crcDigest) {
        MMKVDebug("[%s] oldCrc %u, newCrc %u", m_mmapID.c_str(), m_metaInfo.m_crcDigest,
                  metaInfo.m_crcDigest);
        SCOPEDLOCK(m_sharedProcessLock);

        size_t fileSize = 0;
        if (m_isAshmem) {
            fileSize = m_size;
        } else {
            struct stat st = {0};
            if (fstat(m_fd, &st) != -1) {
                fileSize = (size_t) st.st_size;
            }
        }
        if (m_size != fileSize) {
            MMKVInfo("file size has changed [%s] from %zu to %zu", m_mmapID.c_str(), m_size,
                     fileSize);
            clearMemoryState();
            loadFromFile();
        } else {
            partialLoadFromFile();
        }
        notifyContentChanged();
    }
}

void MMKV::notifyContentChanged() {
    if (g_isContentChangeNotifying) {
        mmkv::onContentChangedByOuterProcess(m_mmapID);
    }
}

void MMKV::checkContentChanged() {
    SCOPEDLOCK(m_lock);
    checkLoadData();
}

void MMKV::clearAll() {
    MMKVInfo("cleaning all key-values from [%s]", m_mmapID.c_str());
    SCOPEDLOCK(m_lock);
    SCOPEDLOCK(m_exclusiveProcessLock);

    if (m_needLoadFromFile && !m_isAshmem) {
        removeFile(m_path.c_str());
        loadFromFile();
        return;
    }

    if (m_ptr && m_ptr != MAP_FAILED) {
        // for truncate
        size_t size = DEFAULT_MMAP_SIZE;
        memset(m_ptr, 0, size);
        if (msync(m_ptr, size, MS_SYNC) != 0) {
            MMKVError("fail to msync [%s]:%s", m_mmapID.c_str(), strerror(errno));
        }
    }
    if (!m_isAshmem) {
        if (m_fd >= 0) {
            if (m_size != DEFAULT_MMAP_SIZE) {
                MMKVInfo("truncating [%s] from %zu to %d", m_mmapID.c_str(), m_size,
                         DEFAULT_MMAP_SIZE);
                if (ftruncate(m_fd, DEFAULT_MMAP_SIZE) != 0) {
                    MMKVError("fail to truncate [%s] to size %d, %s", m_mmapID.c_str(),
                              DEFAULT_MMAP_SIZE, strerror(errno));
                }
            }
        }
    }

    unsigned char newIV[AES_KEY_LEN];
    AESCrypt::fillRandomIV(newIV);
    if (m_crypter) {
        m_crypter->reset(newIV, sizeof(newIV));
    }
    writeActualSize(0, 0, newIV, IncreaseSequence);
    if (m_metaFile.isFileValid()) {
        msync(m_metaFile.getMemory(), DEFAULT_MMAP_SIZE, MS_SYNC);
    }

    clearMemoryState();
    loadFromFile();
}

void MMKV::clearMemoryState() {
    MMKVInfo("clearMemoryState [%s]", m_mmapID.c_str());
    SCOPEDLOCK(m_lock);
    if (m_needLoadFromFile) {
        return;
    }
    m_needLoadFromFile = true;

    m_dic.clear();
    m_hasFullWriteback = false;

    if (m_crypter) {
        if (m_metaInfo.m_version >= MMKVVersionRandomIV) {
            m_crypter->reset(m_metaInfo.m_vector, sizeof(m_metaInfo.m_vector));
        } else {
            m_crypter->reset();
        }
    }

    if (m_output) {
        delete m_output;
    }
    m_output = nullptr;

    if (!m_isAshmem) {
        if (m_ptr && m_ptr != MAP_FAILED) {
            if (munmap(m_ptr, m_size) != 0) {
                MMKVError("fail to munmap [%s], %s", m_mmapID.c_str(), strerror(errno));
            }
        }
        m_ptr = nullptr;

        if (m_fd >= 0) {
            if (::close(m_fd) != 0) {
                MMKVError("fail to close [%s], %s", m_mmapID.c_str(), strerror(errno));
            }
        }
        m_fd = -1;
    }
    m_size = 0;
    m_actualSize = 0;
    m_metaInfo.m_crcDigest = 0;
}

void MMKV::close() {
    MMKVInfo("close [%s]", m_mmapID.c_str());
    SCOPEDLOCK(g_instanceLock);
    SCOPEDLOCK(m_lock);

    auto itr = g_instanceDic->find(m_mmapID);
    if (itr != g_instanceDic->end()) {
        g_instanceDic->erase(itr);
    }
    delete this;
}

void MMKV::trim() {
    if (m_isAshmem) {
        MMKVInfo("there's no way to trim ashmem MMKV:%s", m_mmapID.c_str());
        return;
    }
    SCOPEDLOCK(m_lock);
    MMKVInfo("prepare to trim %s", m_mmapID.c_str());

    checkLoadData();

    if (m_actualSize == 0) {
        clearAll();
        return;
    } else if (m_size <= DEFAULT_MMAP_SIZE) {
        return;
    }
    SCOPEDLOCK(m_exclusiveProcessLock);

    fullWriteback();
    auto oldSize = m_size;
    while (m_size > (m_actualSize + Fixed32Size) * 2) {
        m_size /= 2;
    }
    if (oldSize == m_size) {
        MMKVInfo("there's no need to trim %s with size %zu, actualSize %zu", m_mmapID.c_str(),
                 m_size, m_actualSize);
        return;
    }

    MMKVInfo("trimming %s from %zu to %zu, actualSize %zu", m_mmapID.c_str(), oldSize, m_size,
             m_actualSize);

    if (ftruncate(m_fd, m_size) != 0) {
        MMKVError("fail to truncate [%s] to size %zu, %s", m_mmapID.c_str(), m_size,
                  strerror(errno));
        m_size = oldSize;
        return;
    }
    if (munmap(m_ptr, oldSize) != 0) {
        MMKVError("fail to munmap [%s], %s", m_mmapID.c_str(), strerror(errno));
    }
    m_ptr = (char *) mmap(m_ptr, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
    if (m_ptr == MAP_FAILED) {
        MMKVError("fail to mmap [%s], %s", m_mmapID.c_str(), strerror(errno));
    }

    delete m_output;
    m_output = new CodedOutputData(m_ptr + pbFixed32Size(0), m_size - pbFixed32Size(0));
    m_output->seek(m_actualSize);

    MMKVInfo("finish trim %s from to %zu", m_mmapID.c_str(), m_size);
}

// since we use append mode, when -[setData: forKey:] many times, space may not be enough
// try a full rewrite to make space
bool MMKV::ensureMemorySize(size_t newSize) {
    if (!isFileValid()) {
        MMKVWarning("[%s] file not valid", m_mmapID.c_str());
        return false;
    }

    // make some room for placeholder
    constexpr size_t ItemSizeHolderSize = 4;
    if (m_dic.empty()) {
        newSize += ItemSizeHolderSize;
    }
    if (newSize >= m_output->spaceLeft() || m_dic.empty()) {
        // try a full rewrite to make space
        static const int offset = pbFixed32Size(0);
        MMBuffer data = MiniPBCoder::encodeDataWithObject(m_dic);
        size_t lenNeeded = data.length() + offset + newSize;
        if (m_isAshmem) {
            if (lenNeeded > m_size) {
                MMKVError("ashmem %s reach size limit:%zu, consider configure with larger size",
                          m_mmapID.c_str(), m_size);
                return false;
            }
        } else {
            size_t avgItemSize = lenNeeded / std::max<size_t>(1, m_dic.size());
            size_t futureUsage = avgItemSize * std::max<size_t>(8, (m_dic.size() + 1) / 2);
            // 1. no space for a full rewrite, double it
            // 2. or space is not large enough for future usage, double it to avoid frequently full rewrite
            if (lenNeeded >= m_size || (lenNeeded + futureUsage) >= m_size) {
                size_t oldSize = m_size;
                do {
                    m_size *= 2;
                } while (lenNeeded + futureUsage >= m_size);
                MMKVInfo(
                    "extending [%s] file size from %zu to %zu, incoming size:%zu, future usage:%zu",
                    m_mmapID.c_str(), oldSize, m_size, newSize, futureUsage);

                // if we can't extend size, rollback to old state
                if (ftruncate(m_fd, m_size) != 0) {
                    MMKVError("fail to truncate [%s] to size %zu, %s", m_mmapID.c_str(), m_size,
                              strerror(errno));
                    m_size = oldSize;
                    return false;
                }
                if (!zeroFillFile(m_fd, oldSize, m_size - oldSize)) {
                    MMKVError("fail to zeroFile [%s] to size %zu, %s", m_mmapID.c_str(), m_size,
                              strerror(errno));
                    m_size = oldSize;
                    return false;
                }

                if (munmap(m_ptr, oldSize) != 0) {
                    MMKVError("fail to munmap [%s], %s", m_mmapID.c_str(), strerror(errno));
                }
                m_ptr = (char *) mmap(m_ptr, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
                if (m_ptr == MAP_FAILED) {
                    MMKVError("fail to mmap [%s], %s", m_mmapID.c_str(), strerror(errno));
                }

                // check if we fail to make more space
                if (!isFileValid()) {
                    MMKVWarning("[%s] file not valid", m_mmapID.c_str());
                    return false;
                }
            }
        }
        return doFullWriteBack(std::move(data));
    }
    return true;
}

size_t MMKV::readActualSize() {
    assert(m_ptr != nullptr && m_ptr != MAP_FAILED);
    assert(m_metaFile.isFileValid());

    uint32_t actualSize = 0;
    memcpy(&actualSize, m_ptr, sizeof(actualSize));

    if (m_metaInfo.m_version >= MMKVVersionActualSize) {
        if (m_metaInfo.m_actualSize != actualSize) {
            MMKVWarning("[%s] actual size %u, meta actual size %zu", m_mmapID.c_str(), actualSize,
                        m_metaInfo.m_actualSize);
        }
        return m_metaInfo.m_actualSize;
    } else {
        return actualSize;
    }
}

void MMKV::oldStyleWriteActualSize(size_t actualSize) {
    assert(m_ptr != 0);
    assert(m_ptr != MAP_FAILED);

    m_actualSize = actualSize;
    memcpy(m_ptr, &actualSize, Fixed32Size);
}

bool MMKV::writeActualSize(size_t actualSize,
                           uint32_t crcDigest,
                           const unsigned char *iv,
                           bool increaseSequence) {
    // backward compatibility
    oldStyleWriteActualSize(actualSize);

    if (!m_metaFile.isFileValid()) {
        return false;
    }

    bool needsFullWrite = false;
    m_actualSize = actualSize;
    m_metaInfo.m_actualSize = actualSize;
    m_crcDigest = crcDigest;
    m_metaInfo.m_crcDigest = crcDigest;
    if (m_metaInfo.m_version < MMKVVersionSequence) {
        m_metaInfo.m_version = MMKVVersionSequence;
        needsFullWrite = true;
    }
    if (unlikely(iv)) {
        memcpy(m_metaInfo.m_vector, iv, sizeof(m_metaInfo.m_vector));
        if (m_metaInfo.m_version < MMKVVersionRandomIV) {
            m_metaInfo.m_version = MMKVVersionRandomIV;
        }
        needsFullWrite = true;
    }
    if (unlikely(increaseSequence)) {
        m_metaInfo.m_sequence++;
        m_metaInfo.m_lastConfirmedMetaInfo.lastActualSize = actualSize;
        m_metaInfo.m_lastConfirmedMetaInfo.lastCRCDigest = crcDigest;
        if (m_metaInfo.m_version < MMKVVersionActualSize) {
            m_metaInfo.m_version = MMKVVersionActualSize;
        }
        needsFullWrite = true;
    }
    if (unlikely(needsFullWrite)) {
        m_metaInfo.write(m_metaFile.getMemory());
    } else {
        m_metaInfo.writeCRCAndActualSizeOnly(m_metaFile.getMemory());
    }

    return true;
}

const MMBuffer &MMKV::getDataForKey(const std::string &key) {
    checkLoadData();
    auto itr = m_dic.find(key);
    if (itr != m_dic.end()) {
        return itr->second;
    }
    static MMBuffer nan(0);
    return nan;
}

bool MMKV::setDataForKey(MMBuffer &&data, const std::string &key) {
    if (data.length() == 0 || key.empty()) {
        return false;
    }
    SCOPEDLOCK(m_lock);
    SCOPEDLOCK(m_exclusiveProcessLock);
    checkLoadData();

    auto ret = appendDataWithKey(data, key);
    if (ret) {
        m_dic[key] = std::move(data);
        m_hasFullWriteback = false;
    }
    return ret;
}

bool MMKV::removeDataForKey(const std::string &key) {
    if (key.empty()) {
        return false;
    }

    auto deleteCount = m_dic.erase(key);
    if (deleteCount > 0) {
        m_hasFullWriteback = false;
        static MMBuffer nan(0);
        return appendDataWithKey(nan, key);
    }

    return false;
}

bool MMKV::appendDataWithKey(const MMBuffer &data, const std::string &key) {
    size_t keyLength = key.length();
    // size needed to encode the key
    size_t size = keyLength + pbRawVarint32Size((int32_t) keyLength);
    // size needed to encode the value
    size += data.length() + pbRawVarint32Size((int32_t) data.length());

    SCOPEDLOCK(m_exclusiveProcessLock);

    bool hasEnoughSize = ensureMemorySize(size);

    if (!hasEnoughSize || !isFileValid()) {
        return false;
    }

    m_output->writeString(key);
    m_output->writeData(data); // note: write size of data

    auto ptr = (uint8_t *) m_ptr + Fixed32Size + m_actualSize;
    if (m_crypter) {
        m_crypter->encrypt(ptr, ptr, size);
    }
    m_actualSize += size;
    updateCRCDigest(ptr, size);

    return true;
}

bool MMKV::fullWriteback() {
    if (m_hasFullWriteback) {
        return true;
    }
    if (m_needLoadFromFile) {
        return true;
    }
    if (!isFileValid()) {
        MMKVWarning("[%s] file not valid", m_mmapID.c_str());
        return false;
    }

    if (m_dic.empty()) {
        clearAll();
        return true;
    }

    auto allData = MiniPBCoder::encodeDataWithObject(m_dic);
    SCOPEDLOCK(m_exclusiveProcessLock);
    if (allData.length() > 0) {
        if (allData.length() + Fixed32Size <= m_size) {
            return doFullWriteBack(std::move(allData));
        } else {
            // ensureMemorySize will extend file & full rewrite, no need to write back again
            return ensureMemorySize(allData.length() + Fixed32Size - m_size);
        }
    }
    return false;
}

bool MMKV::doFullWriteBack(MMBuffer &&allData) {
    unsigned char newIV[AES_KEY_LEN];
    if (m_crypter) {
        AESCrypt::fillRandomIV(newIV);
        m_crypter->reset(newIV, sizeof(newIV));
        auto ptr = (unsigned char *) allData.getPtr();
        m_crypter->encrypt(ptr, ptr, allData.length());
    }

    constexpr auto offset = pbFixed32Size(0);
    delete m_output;
    m_output = new CodedOutputData(m_ptr + offset, m_size - offset);
    m_output->writeRawData(allData); // note: don't write size of data
    m_actualSize = allData.length();
    if (m_crypter) {
        recaculateCRCDigestWithIV(newIV);
    } else {
        recaculateCRCDigestWithIV(nullptr);
    }
    m_hasFullWriteback = true;
    // make sure lastConfirmedMetaInfo is saved
    sync(true);
    return true;
}

bool MMKV::reKey(const std::string &cryptKey) {
    SCOPEDLOCK(m_lock);
    checkLoadData();

    if (m_crypter) {
        if (cryptKey.length() > 0) {
            string oldKey = this->cryptKey();
            if (cryptKey == oldKey) {
                return true;
            } else {
                // change encryption key
                MMKVInfo("reKey with new aes key");
                delete m_crypter;
                auto ptr = (const unsigned char *) cryptKey.data();
                m_crypter = new AESCrypt(ptr, cryptKey.length());
                return fullWriteback();
            }
        } else {
            // decryption to plain text
            MMKVInfo("reKey with no aes key");
            delete m_crypter;
            m_crypter = nullptr;
            return fullWriteback();
        }
    } else {
        if (cryptKey.length() > 0) {
            // transform plain text to encrypted text
            MMKVInfo("reKey with aes key");
            auto ptr = (const unsigned char *) cryptKey.data();
            m_crypter = new AESCrypt(ptr, cryptKey.length());
            return fullWriteback();
        } else {
            return true;
        }
    }
    return false;
}

void MMKV::checkReSetCryptKey(const std::string *cryptKey) {
    SCOPEDLOCK(m_lock);

    if (m_crypter) {
        if (cryptKey) {
            string oldKey = this->cryptKey();
            if (oldKey != *cryptKey) {
                MMKVInfo("setting new aes key");
                delete m_crypter;
                auto ptr = (const unsigned char *) cryptKey->data();
                m_crypter = new AESCrypt(ptr, cryptKey->length());

                checkLoadData();
            } else {
                // nothing to do
            }
        } else {
            MMKVInfo("reset aes key");
            delete m_crypter;
            m_crypter = nullptr;

            checkLoadData();
        }
    } else {
        if (cryptKey) {
            MMKVInfo("setting new aes key");
            auto ptr = (const unsigned char *) cryptKey->data();
            m_crypter = new AESCrypt(ptr, cryptKey->length());

            checkLoadData();
        } else {
            // nothing to do
        }
    }
}

void MMKV::checkReSetCryptKey(int fd, int metaFD, std::string *cryptKey) {
    SCOPEDLOCK(m_lock);

    checkReSetCryptKey(cryptKey);

    if (m_isAshmem) {
        if (m_fd != fd) {
            ::close(fd);
        }
        if (m_metaFile.getFd() != metaFD) {
            ::close(metaFD);
        }
    }
}

bool MMKV::isFileValid() {
    if (m_fd >= 0 && m_size > 0 && m_output && m_ptr && m_ptr != MAP_FAILED) {
        return true;
    }
    return false;
}

#pragma mark - crc

// assuming m_ptr & m_size is set
bool MMKV::checkFileCRCValid(size_t acutalSize, uint32_t crcDigest) {
    if (m_ptr != nullptr && m_ptr != MAP_FAILED) {
        constexpr auto offset = pbFixed32Size(0);
        m_crcDigest =
            (uint32_t) zlib::crc32(0, (const uint8_t *) m_ptr + offset, (uint32_t) acutalSize);

        if (m_crcDigest == crcDigest) {
            return true;
        }
        MMKVError("check crc [%s] fail, crc32:%u, m_crcDigest:%u", m_mmapID.c_str(), crcDigest,
                  m_crcDigest);
    }
    return false;
}

void MMKV::recaculateCRCDigestWithIV(const unsigned char *iv) {
    if (m_ptr != nullptr && m_ptr != MAP_FAILED) {
        constexpr auto offset = pbFixed32Size(0);
        m_crcDigest = 0;
        m_crcDigest =
            (uint32_t) zlib::crc32(0, (const uint8_t *) m_ptr + offset, (uint32_t) m_actualSize);
        writeActualSize(m_actualSize, m_crcDigest, iv, IncreaseSequence);
    }
}

void MMKV::updateCRCDigest(const uint8_t *ptr, size_t length) {
    if (ptr == nullptr) {
        return;
    }
    m_crcDigest = (uint32_t) zlib::crc32(m_crcDigest, ptr, (uint32_t) length);

    writeActualSize(m_actualSize, m_crcDigest, nullptr, KeepSequence);
}

#pragma mark - set & get

bool MMKV::setStringForKey(const std::string &value, const std::string &key) {
    if (key.empty()) {
        return false;
    }
    auto data = MiniPBCoder::encodeDataWithObject(value);
    return setDataForKey(std::move(data), key);
}

bool MMKV::setBytesForKey(const MMBuffer &value, const std::string &key) {
    if (key.empty()) {
        return false;
    }
    auto data = MiniPBCoder::encodeDataWithObject(value);
    return setDataForKey(std::move(data), key);
}

bool MMKV::setBool(bool value, const std::string &key) {
    if (key.empty()) {
        return false;
    }
    size_t size = pbBoolSize(value);
    MMBuffer data(size);
    CodedOutputData output(data.getPtr(), size);
    output.writeBool(value);

    return setDataForKey(std::move(data), key);
}

bool MMKV::setInt32(int32_t value, const std::string &key) {
    if (key.empty()) {
        return false;
    }
    size_t size = pbInt32Size(value);
    MMBuffer data(size);
    CodedOutputData output(data.getPtr(), size);
    output.writeInt32(value);

    return setDataForKey(std::move(data), key);
}

bool MMKV::setInt64(int64_t value, const std::string &key) {
    if (key.empty()) {
        return false;
    }
    size_t size = pbInt64Size(value);
    MMBuffer data(size);
    CodedOutputData output(data.getPtr(), size);
    output.writeInt64(value);

    return setDataForKey(std::move(data), key);
}

bool MMKV::setFloat(float value, const std::string &key) {
    if (key.empty()) {
        return false;
    }
    size_t size = pbFloatSize(value);
    MMBuffer data(size);
    CodedOutputData output(data.getPtr(), size);
    output.writeFloat(value);

    return setDataForKey(std::move(data), key);
}

bool MMKV::setDouble(double value, const std::string &key) {
    if (key.empty()) {
        return false;
    }
    size_t size = pbDoubleSize(value);
    MMBuffer data(size);
    CodedOutputData output(data.getPtr(), size);
    output.writeDouble(value);

    return setDataForKey(std::move(data), key);
}

bool MMKV::setVectorForKey(const std::vector<std::string> &v, const std::string &key) {
    if (key.empty()) {
        return false;
    }
    auto data = MiniPBCoder::encodeDataWithObject(v);
    return setDataForKey(std::move(data), key);
}

bool MMKV::getStringForKey(const std::string &key, std::string &result) {
    if (key.empty()) {
        return false;
    }
    SCOPEDLOCK(m_lock);
    auto &data = getDataForKey(key);
    if (data.length() > 0) {
        result = MiniPBCoder::decodeString(data);
        return true;
    }
    return false;
}

MMBuffer MMKV::getBytesForKey(const std::string &key) {
    if (key.empty()) {
        return MMBuffer(0);
    }
    SCOPEDLOCK(m_lock);
    auto &data = getDataForKey(key);
    if (data.length() > 0) {
        return MiniPBCoder::decodeBytes(data);
    }
    return MMBuffer(0);
}

bool MMKV::getBoolForKey(const std::string &key, bool defaultValue) {
    if (key.empty()) {
        return defaultValue;
    }
    SCOPEDLOCK(m_lock);
    auto &data = getDataForKey(key);
    if (data.length() > 0) {
        CodedInputData input(data.getPtr(), data.length());
        return input.readBool();
    }
    return defaultValue;
}

int32_t MMKV::getInt32ForKey(const std::string &key, int32_t defaultValue) {
    if (key.empty()) {
        return defaultValue;
    }
    SCOPEDLOCK(m_lock);
    auto &data = getDataForKey(key);
    if (data.length() > 0) {
        CodedInputData input(data.getPtr(), data.length());
        return input.readInt32();
    }
    return defaultValue;
}

int64_t MMKV::getInt64ForKey(const std::string &key, int64_t defaultValue) {
    if (key.empty()) {
        return defaultValue;
    }
    SCOPEDLOCK(m_lock);
    auto &data = getDataForKey(key);
    if (data.length() > 0) {
        CodedInputData input(data.getPtr(), data.length());
        return input.readInt64();
    }
    return defaultValue;
}

float MMKV::getFloatForKey(const std::string &key, float defaultValue) {
    if (key.empty()) {
        return defaultValue;
    }
    SCOPEDLOCK(m_lock);
    auto &data = getDataForKey(key);
    if (data.length() > 0) {
        CodedInputData input(data.getPtr(), data.length());
        return input.readFloat();
    }
    return defaultValue;
}

double MMKV::getDoubleForKey(const std::string &key, double defaultValue) {
    if (key.empty()) {
        return defaultValue;
    }
    SCOPEDLOCK(m_lock);
    auto &data = getDataForKey(key);
    if (data.length() > 0) {
        CodedInputData input(data.getPtr(), data.length());
        return input.readDouble();
    }
    return defaultValue;
}

bool MMKV::getVectorForKey(const std::string &key, std::vector<std::string> &result) {
    if (key.empty()) {
        return false;
    }
    SCOPEDLOCK(m_lock);
    auto &data = getDataForKey(key);
    if (data.length() > 0) {
        result = MiniPBCoder::decodeSet(data);
        return true;
    }
    return false;
}

size_t MMKV::getValueSizeForKey(const std::string &key, bool actualSize) {
    if (key.empty()) {
        return 0;
    }
    SCOPEDLOCK(m_lock);
    auto &data = getDataForKey(key);
    if (actualSize) {
        CodedInputData input(data.getPtr(), data.length());
        auto length = input.readInt32();
        if (pbRawVarint32Size(length) + length == data.length()) {
            return static_cast<size_t>(length);
        }
    }
    return data.length();
}

int32_t MMKV::writeValueToBuffer(const std::string &key, void *ptr, int32_t size) {
    if (key.empty()) {
        return -1;
    }
    SCOPEDLOCK(m_lock);
    auto &data = getDataForKey(key);
    CodedInputData input(data.getPtr(), data.length());
    auto length = input.readInt32();
    auto offset = pbRawVarint32Size(length);
    if (offset + length == data.length()) {
        if (length <= size) {
            memcpy(ptr, (uint8_t *) data.getPtr() + offset, length);
            return length;
        }
    } else {
        if (data.length() <= size) {
            memcpy(ptr, data.getPtr(), data.length());
            return static_cast<int32_t>(data.length());
        }
    }
    return -1;
}

#pragma mark - enumerate

bool MMKV::containsKey(const std::string &key) {
    SCOPEDLOCK(m_lock);
    checkLoadData();
    return m_dic.find(key) != m_dic.end();
}

size_t MMKV::count() {
    SCOPEDLOCK(m_lock);
    checkLoadData();
    return m_dic.size();
}

size_t MMKV::totalSize() {
    SCOPEDLOCK(m_lock);
    checkLoadData();
    return m_size;
}

std::vector<std::string> MMKV::allKeys() {
    SCOPEDLOCK(m_lock);
    checkLoadData();

    vector<string> keys;
    for (const auto &itr : m_dic) {
        keys.push_back(itr.first);
    };
    return keys;
}

void MMKV::removeValueForKey(const std::string &key) {
    if (key.empty()) {
        return;
    }
    SCOPEDLOCK(m_lock);
    SCOPEDLOCK(m_exclusiveProcessLock);
    checkLoadData();

    removeDataForKey(key);
}

void MMKV::removeValuesForKeys(const std::vector<std::string> &arrKeys) {
    if (arrKeys.empty()) {
        return;
    }
    if (arrKeys.size() == 1) {
        return removeValueForKey(arrKeys[0]);
    }

    SCOPEDLOCK(m_lock);
    SCOPEDLOCK(m_exclusiveProcessLock);
    checkLoadData();
    for (const auto &key : arrKeys) {
        m_dic.erase(key);
    }
    m_hasFullWriteback = false;

    fullWriteback();
}

#pragma mark - file

void MMKV::sync(bool sync) {
    SCOPEDLOCK(m_lock);
    if (m_needLoadFromFile || !isFileValid()) {
        return;
    }
    SCOPEDLOCK(m_exclusiveProcessLock);
    auto flag = sync ? MS_SYNC : MS_ASYNC;
    if (msync(m_ptr, m_size, flag) != 0) {
        MMKVError("fail to msync[%d] [%s]:%s", flag, m_mmapID.c_str(), strerror(errno));
    }
    if (m_metaFile.isFileValid()) {
        if (msync(m_metaFile.getMemory(), DEFAULT_MMAP_SIZE, flag) != 0) {
            MMKVError("fail to msync[%d] [%s]:%s", flag, m_metaFile.getName().c_str(),
                      strerror(errno));
        }
    }
}

bool MMKV::isFileValid(const std::string &mmapID) {
    string kvPath = mappedKVPathWithID(mmapID, MMKV_SINGLE_PROCESS);
    if (!isFileExist(kvPath)) {
        return true;
    }

    string crcPath = crcPathWithID(mmapID, MMKV_SINGLE_PROCESS);
    if (!isFileExist(crcPath.c_str())) {
        return false;
    }

    uint32_t crcFile = 0;
    MMBuffer *data = readWholeFile(crcPath.c_str());
    if (data) {
        if (data->getPtr()) {
            MMKVMetaInfo metaInfo;
            metaInfo.read(data->getPtr());
            crcFile = metaInfo.m_crcDigest;
        }
        delete data;
    } else {
        return false;
    }

    const int offset = pbFixed32Size(0);
    uint32_t crcDigest = 0;
    MMBuffer *fileData = readWholeFile(kvPath.c_str());
    if (fileData) {
        if (fileData->getPtr()) {
            size_t actualSize =
                CodedInputData(fileData->getPtr(), fileData->length()).readFixed32();
            if (actualSize > fileData->length() - offset) {
                delete fileData;
                return false;
            }

            crcDigest = (uint32_t) zlib::crc32(0, (const uint8_t *) fileData->getPtr() + offset,
                                               (uint32_t) actualSize);
        }
        delete fileData;
        return crcFile == crcDigest;
    } else {
        return false;
    }
}

static void mkSpecialCharacterFileDirectory() {
    char *path = strdup((g_rootDir + "/" + SPECIAL_CHARACTER_DIRECTORY_NAME).c_str());
    if (path) {
        mkPath(path);
        free(path);
    }
}

static string md5(const string &value) {
    unsigned char md[MD5_DIGEST_LENGTH] = {0};
    char tmp[3] = {0}, buf[33] = {0};
    MD5((const unsigned char *) value.c_str(), value.size(), md);
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        snprintf(tmp, sizeof(tmp), "%2.2x", md[i]);
        strcat(buf, tmp);
    }
    return string(buf);
}

static string encodeFilePath(const string &mmapID) {
    const char *specialCharacters = "\\/:*?\"<>|";
    string encodedID;
    bool hasSpecialCharacter = false;
    for (int i = 0; i < mmapID.size(); i++) {
        if (strchr(specialCharacters, mmapID[i]) != NULL) {
            encodedID = md5(mmapID);
            hasSpecialCharacter = true;
            break;
        }
    }
    if (hasSpecialCharacter) {
        static pthread_once_t once_control = PTHREAD_ONCE_INIT;
        pthread_once(&once_control, mkSpecialCharacterFileDirectory);
        return string(SPECIAL_CHARACTER_DIRECTORY_NAME) + "/" + encodedID;
    } else {
        return mmapID;
    }
}

static string mmapedKVKey(const string &mmapID, string *relativePath) {
    if (relativePath && g_rootDir != (*relativePath)) {
        return md5(*relativePath + "/" + mmapID);
    }
    return mmapID;
}

static string mappedKVPathWithID(const string &mmapID, MMKVMode mode, string *relativePath) {
    if (mode & MMKV_ASHMEM) {
        return string(ASHMEM_NAME_DEF) + "/" + encodeFilePath(mmapID);
    } else if (relativePath) {
        return *relativePath + "/" + encodeFilePath(mmapID);
    }
    return g_rootDir + "/" + encodeFilePath(mmapID);
}

static string crcPathWithID(const string &mmapID, MMKVMode mode, string *relativePath) {
    if (mode & MMKV_ASHMEM) {
        return encodeFilePath(mmapID) + ".crc";
    } else if (relativePath) {
        return *relativePath + "/" + encodeFilePath(mmapID) + ".crc";
    }
    return g_rootDir + "/" + encodeFilePath(mmapID) + ".crc";
}
