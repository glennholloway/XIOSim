#include <sys/statvfs.h>

#include <unordered_map>
#include <sstream>

#include "pin.H"

#include "boost_interprocess.h"

#include "feeder.h"
#include "multiprocess_shared.h"
#include "ipc_queues.h"
#include "BufferManagerProducer.h"


namespace xiosim
{
namespace buffer_management
{

static void copyProducerToFile(pid_t tid, bool checkSpace);
static void copyProducerToFileReal(pid_t tid, bool checkSpace);
static void copyProducerToFileFake(pid_t tid);
static void writeHandshake(pid_t tid, int fd, handshake_container_t* handshake);
static int getKBFreeSpace(boost::interprocess::string path);
static void reserveHandshake(pid_t tid);
static shm_string genFileName(boost::interprocess::string path);

static std::unordered_map<pid_t, Buffer*> produceBuffer_;
static std::unordered_map<pid_t, int> writeBufferSize_;
static std::unordered_map<pid_t, void*> writeBuffer_;
static std::unordered_map<pid_t, regs_t*> shadowRegs_;
static std::vector<boost::interprocess::string> bridgeDirs_;
static boost::interprocess::string gpid_;

void InitBufferManagerProducer(pid_t harness_pid, int num_cores)
{
    InitBufferManager(harness_pid, num_cores);

    produceBuffer_.reserve(MAX_CORES);
    writeBufferSize_.reserve(MAX_CORES);
    writeBuffer_.reserve(MAX_CORES);
    shadowRegs_.reserve(MAX_CORES);

    bridgeDirs_.push_back("/dev/shm/");
    bridgeDirs_.push_back("/tmp/");

    int pid = getpgrp();
    ostringstream iss;
    iss << pid;
    gpid_ = iss.str().c_str();
    assert(gpid_.length() > 0);
    cerr << " Creating temp files with prefix "  << gpid_ << "_*" << endl;
}

void DeinitBufferManagerProducer()
{
    DeinitBufferManager();

    for(int i = 0; i < (int)bridgeDirs_.size(); i++) {
        boost::interprocess::string cmd = "/bin/rm -rf " + bridgeDirs_[i] + gpid_ + "_* &";
        int retVal = system(cmd.c_str());
        (void)retVal;
        assert(retVal == 0);
    }
}

void AllocateThreadProducer(pid_t tid)
{
    int bufferCapacity = AllocateThread(tid);

    produceBuffer_[tid] = new Buffer(bufferCapacity);
    resetPool(tid);
    writeBufferSize_[tid] = 4096;
    writeBuffer_[tid] = malloc(4096);
    assert(writeBuffer_[tid]);
    shadowRegs_[tid] = (regs_t*)calloc(1, sizeof(regs_t));

    /* send IPC message to allocate consumer-side */
    ipc_message_t msg;
    msg.BufferManagerAllocateThread(tid, bufferCapacity);
    SendIPCMessage(msg);
}

handshake_container_t* back(pid_t tid)
{
    lk_lock(&locks_[tid], tid+1);
    assert(queueSizes_[tid] > 0);
    handshake_container_t* returnVal = produceBuffer_[tid]->back();
    lk_unlock(&locks_[tid]);
    return returnVal;
}

/* On the producer side, get a buffer which we can start
 * filling directly.
 */
handshake_container_t* get_buffer(pid_t tid)
{
  lk_lock(&locks_[tid], tid+1);
  // Push is guaranteed to succeed because each call to
  // get_buffer() is followed by a call to producer_done()
  // which will make space if full
  handshake_container_t* result = produceBuffer_[tid]->get_buffer();
  produceBuffer_[tid]->push_done();
  queueSizes_[tid]++;
  assert(pool_[tid] > 0);
  pool_[tid]--;

  lk_unlock(&locks_[tid]);
  return result;
}

/* On the producer side, signal that we are done filling the
 * current buffer. If we have ran out of space, make space
 * for a new buffer, so get_buffer() cannot fail.
 */
void producer_done(pid_t tid, bool keepLock)
{
  lk_lock(&locks_[tid], tid+1);

  ASSERTX(!produceBuffer_[tid]->empty());
  handshake_container_t* last = produceBuffer_[tid]->back();
  ASSERTX(last->flags.valid);

  if(!keepLock) {
    reserveHandshake(tid);
  }
  else {
    pool_[tid]++; // Expand in case the last handshakes need space
  }

  if(produceBuffer_[tid]->full()) {// || ( (consumeBuffer_[tid]->size() == 0) && (fileEntryCount_[tid] == 0))) {
#if defined(DEBUG) || defined(ZESTO_PIN_DBG)
    int produceSize = produceBuffer_[tid]->size();
#endif
    bool checkSpace = !keepLock;
    copyProducerToFile(tid, checkSpace);
    assert(fileEntryCount_[tid] > 0);
    assert(fileEntryCount_[tid] >= produceSize);
    assert(produceBuffer_[tid]->size() == 0);
  }

  assert(!produceBuffer_[tid]->full());

  lk_unlock(&locks_[tid]);
}

/* On the producer side, flush all buffers associated
 * with a thread to the backing file.
 */
void flushBuffers(pid_t tid)
{
  lk_lock(&locks_[tid], tid+1);

  if(produceBuffer_[tid]->size() > 0) {
    copyProducerToFile(tid, false);
  }
  lk_unlock(&locks_[tid]);
}

void resetPool(pid_t tid)
{
  int poolFactor = 3;
  assert(poolFactor >= 1);
  /* Assume produceBuffer->capacity == consumeBuffer->capacity */
  pool_[tid] = (2 * produceBuffer_[tid]->capacity()) * poolFactor;
  //  pool_[tid] = 2000000000;
}

/* On the producer side, if we have filled up the in-memory
 * buffer, wait until some of it gets consumed. If not,
 * try and increase the backing file size.
 */
static void reserveHandshake(pid_t tid)
{
  if(pool_[tid] > 0) {
    return;
  }

  //  while(pool_[tid] == 0) {
  while(true) {
    assert(queueSizes_[tid] > 0);
    lk_unlock(&locks_[tid]);

    enable_consumers();
    disable_producers();

    PIN_Sleep(500);

    lk_lock(&locks_[tid], tid+1);

    if(*popped_) {
      *popped_ = false;
      continue;
    }

    disable_consumers();
    enable_producers();

    if(pool_[tid] > 0) {
      break;
    }

    if(KnobNumCores.Value() == 1) {
      continue;
    }

    pool_[tid] += 50000;
#ifdef ZESTO_PIN_DBG
    cerr << tid << " [reserveHandshake()]: Increasing file up to " << queueSizes_[tid] + pool_[tid] << endl;
#endif
    break;
  }
}

static void copyProducerToFile(pid_t tid, bool checkSpace)
{
  if(*useRealFile_) {
    copyProducerToFileReal(tid, checkSpace);
  }
  else {
    copyProducerToFileFake(tid);
  }
}

static void copyProducerToFileFake(pid_t tid)
{
  while(produceBuffer_[tid]->size() > 0) {
    handshake_container_t* handshake = produceBuffer_[tid]->front();
    handshake_container_t* handfake = fakeFile_[tid]->get_buffer();
    handshake->CopyTo(handfake);
    fakeFile_[tid]->push_done();

    produceBuffer_[tid]->pop();
    fileEntryCount_[tid]++;
  }
}

static void copyProducerToFileReal(pid_t tid, bool checkSpace)
{
  int result;
  bool madeFile = false;
  if(checkSpace) {
    for(int i = 0; i < (int)bridgeDirs_.size(); i++) {
      int space = getKBFreeSpace(bridgeDirs_[i]);
      if(space > 2500000) { // 2.5 GB
        fileNames_[tid].push_back(genFileName(bridgeDirs_[i]));
        madeFile = true;
        break;
      }
      //cerr << "Out of space on " + bridgeDirs_[i] + " !!!" << endl;
    }
    if(madeFile == false) {
      cerr << "Nowhere left for the poor file bridge :(" << endl;
      cerr << "BridgeDirs:" << endl;
      for(int i = 0; i < (int)bridgeDirs_.size(); i++) {
        int space = getKBFreeSpace(bridgeDirs_[i]);
        cerr << bridgeDirs_[i] << ":" << space << " in KB" << endl;
      }
      abort();
    }
  }
  else {
    fileNames_[tid].push_back(genFileName(bridgeDirs_[0]));
  }

  fileCounts_[tid].push_back(0);

  int fd = open(fileNames_[tid].back().c_str(), O_WRONLY | O_CREAT, 0777);
  if(fd == -1) {
    cerr << "Opened to write: " << fileNames_[tid].back();
    cerr << "Pipe open error: " << fd << " Errcode:" << strerror(errno) << endl;
    abort();
  }

  while(!produceBuffer_[tid]->empty()) {
    writeHandshake(tid, fd, produceBuffer_[tid]->front());
    produceBuffer_[tid]->pop();
    fileCounts_[tid].back() += 1;
    fileEntryCount_[tid]++;
  }

  result = close(fd);
  if(result != 0) {
    cerr << "Close error: " << " Errcode:" << strerror(errno) << endl;
    abort();
  }

  // sync() if we put the file somewhere besides /dev/shm
  if(fileNames_[tid].back().find("shm") == shm_string::npos) {
    sync();
  }

  assert(produceBuffer_[tid]->size() == 0);
  assert(fileEntryCount_[tid] >= 0);
}

static ssize_t do_write(const int fd, const void* buff, const size_t size)
{
  ssize_t bytesWritten = 0;
  do {
    ssize_t res = write(fd, (void*)((char*)buff + bytesWritten), size - bytesWritten);
    if(res == -1) {
      cerr << "failed write!" << endl;
      cerr << "bytesWritten:" << bytesWritten << endl;
      cerr << "size:" << size << endl;
      return -1;
    }
    bytesWritten += res;
  } while (bytesWritten < (ssize_t)size);
  return bytesWritten;
}

static void writeHandshake(pid_t tid, int fd, handshake_container_t* handshake)
{
  void * writeBuffer = writeBuffer_[tid];
  regs_t * const shadow_regs = shadowRegs_[tid];
  size_t totalBytes = handshake->Serialize(writeBuffer, 4096, shadow_regs);

  ssize_t bytesWritten = do_write(fd, writeBuffer, totalBytes);
  if(bytesWritten == -1) {
    cerr << "Pipe write error: " << bytesWritten << " Errcode:" << strerror(errno) << endl;

    cerr << "Opened to write: " << fileNames_[tid].back() << endl;
    cerr << "Thread Id:" << tid << endl;
    cerr << "fd:" << fd << endl;
    cerr << "Queue Size:" << queueSizes_[tid] << endl;
    cerr << "ProduceBuffer size:" << produceBuffer_[tid]->size() << endl;
    cerr << "file entry count:" << fileEntryCount_[tid] << endl;

    cerr << "BridgeDirs:" << endl;
    for(int i = 0; i < (int)bridgeDirs_.size(); i++) {
      int space = getKBFreeSpace(bridgeDirs_[i]);
      cerr << bridgeDirs_[i] << ":" << space << " in KB" << endl;
    }
    abort();
  }
  if(bytesWritten != (ssize_t)totalBytes) {
    cerr << "File write error: " << bytesWritten << " expected:" << totalBytes << endl;
    cerr << fileNames_[tid].back() << endl;
    abort();
  }

  /* This is ugly and maybe costly. Update the shadow copy.
   * If we care enough, we should double-buffer */
  memcpy(shadow_regs, &(handshake->handshake.ctxt), sizeof(regs_t));
}

static int getKBFreeSpace(boost::interprocess::string path)
{
  struct statvfs fsinfo;
  statvfs(path.c_str(), &fsinfo);
  return ((unsigned long long)fsinfo.f_bsize * (unsigned long long)fsinfo.f_bavail / 1024);
}

static shm_string genFileName(boost::interprocess::string path)
{
  char* temp = tempnam(path.c_str(), gpid_.c_str());
  boost::interprocess::string res = boost::interprocess::string(temp);
  assert(res.find(path) != boost::interprocess::string::npos);
  res.insert(path.length() + gpid_.length(), "_");
  res = res + ".xiosim";

  shm_string shared_res(res.c_str(), global_shm->get_allocator<void>());
  free(temp);
  return shared_res;
}

}
}
