#include "BufferManager.h"


#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

ostream& operator<< (ostream &out, handshake_container_t &hand)
{
  out << "hand:" << " ";
  out << hand.flags.valid;
  out << hand.flags.mem_released;
  out << hand.flags.isFirstInsn;
  out << hand.flags.isLastInsn;
  out << hand.flags.killThread;
  out << " ";
  out << hand.mem_buffer.size();
  out << " ";
  out << "pc:" << hand.handshake.pc << " "; 
  out << "coreid:" << hand.handshake.coreID << " "; 
  out << "npc:" << hand.handshake.npc << " "; 
  out << "tpc:" << hand.handshake.tpc << " "; 
  out << "brtaken:" << hand.handshake.brtaken << " "; 
  out << "ins:" << hand.handshake.ins << " ";
  out << "flags:" << hand.handshake.sleep_thread << hand.handshake.resume_thread;
  out << hand.handshake.iteration_correction << hand.handshake.real;
  out << hand.handshake.in_critical_section;
  out << " slicenum:" << hand.handshake.slice_num << " ";
  out << "feederslicelen:" << hand.handshake.feeder_slice_length << " ";
  out << "feedersliceweight:" << hand.handshake.slice_weight_times_1000 << " ";
  out.flush();
  return out;
}

BufferManager::BufferManager()
{  
  useRealFile_ = true;
}

BufferManager::~BufferManager()
{
  map<THREADID, string>::iterator it;
  for(it = fileNames_.begin(); it != fileNames_.end(); it++) {
    string cmd = "/bin/rm -rf " + fileNames_[it->first] + " " + bogusNames_[it->first];
    system(cmd.c_str());
  }
}

handshake_container_t* BufferManager::front(THREADID tid)
{
  checkFirstAccess(tid);
  //  (*logs_[tid]) << "front before lock" << endl;
  GetLock(locks_[tid], tid+1);
  //  (*logs_[tid]) << "front after lock" << endl;

  assert(queueSizes_[tid] > 0);

  if(consumeBuffer_[tid]->size() > 0) {
    handshake_container_t* returnVal = consumeBuffer_[tid]->front();
    ReleaseLock(locks_[tid]);
    //    GetLock(&simbuffer_lock, tid+1);
    return returnVal;
  }
  assert(fileEntryCount_[tid] > 0 || produceBuffer_[tid]->size() > 0);

  if(fileEntryCount_[tid] > 0) {
    copyFileToConsumer(tid);
    assert(!consumeBuffer_[tid]->empty());    
    handshake_container_t* returnVal = consumeBuffer_[tid]->front();
    ReleaseLock(locks_[tid]);
    //    GetLock(&simbuffer_lock, tid+1);
    return returnVal;
  }
  assert(fileEntryCount_[tid] == 0);
  assert(consumeBuffer_[tid]->empty());
  //  (*logs_[tid]) << "start spins" << endl;
  long long int spins = 0;  
  while(consumeBuffer_[tid]->empty()) {
    // (*logs_[tid]) << "s:0" << endl;
    ReleaseLock(locks_[tid]);    	
    //    ReleaseLock(&simbuffer_lock);
    //    (*logs_[tid]) << "s:1" << endl;
    PIN_Yield();
    //    (*logs_[tid]) << "s:2" << endl;
    //    GetLock(&simbuffer_lock, tid+1);
    //    (*logs_[tid]) << "s:3" << endl;
    GetLock(locks_[tid], tid+1);
    //    (*logs_[tid]) << "s:4" << endl;
    spins++;
    if(spins >= 10LL) {
      cerr << tid << " [front()]: That's a lot of spins!" << endl;
      spins = 0;
      //  cerr << "psize:" << produceBuffer_[tid]->size() << endl;
      // cerr << "fsize:" << fileEntryCount_[tid] << endl;
      //cerr << "csize:" << consumeBuffer_[tid]->size() << endl;
      copyProducerToFile(tid);
      cerr << "midcopy" << endl;
      copyFileToConsumer(tid);
      cerr << "after copy" << endl;
      cerr << "psize:" << produceBuffer_[tid]->size() << endl;
      cerr << "fsize:" << fileEntryCount_[tid] << endl;
      cerr << "csize:" << consumeBuffer_[tid]->size() << endl;
      
      /*      int tmptid = 13;
      if(tid == 13) {
	tmptid = 15;
      }
      cerr << "extra for other core:" << endl;
      cerr << "psize:" << produceBuffer_[tmptid]->size() << endl;
      cerr << "fsize:" << fileEntryCount_[tmptid] << endl;
      cerr << "csize:" << consumeBuffer_[tmptid]->size() << endl;
      cerr << endl;*/
    }
    //    (*logs_[tid]) << "s:5" << endl;
  }
  //  (*logs_[tid]) << "end spins" << endl;
  assert(consumeBuffer_[tid]->size() > 0);
  assert(consumeBuffer_[tid]->front()->flags.valid);
  assert(queueSizes_[tid] > 0);
  handshake_container_t* resultVal = consumeBuffer_[tid]->front();
  ReleaseLock(locks_[tid]);
  PIN_Yield();
  //  GetLock(&simbuffer_lock, tid+1);
  return resultVal;
}

handshake_container_t* BufferManager::back(THREADID tid)
{
  checkFirstAccess(tid);
  GetLock(locks_[tid], tid+1);
  assert(queueSizes_[tid] > 0);
  assert(produceBuffer_[tid]->size() > 0);
  handshake_container_t* returnVal = produceBuffer_[tid]->back();
  ReleaseLock(locks_[tid]);
  return returnVal;
}

bool BufferManager::empty(THREADID tid)
{
  checkFirstAccess(tid);   
  GetLock(locks_[tid], tid+1);
  bool result = queueSizes_[tid] == 0;
  ReleaseLock(locks_[tid]);
  return result;
}

void BufferManager::push(THREADID tid, handshake_container_t* handshake, bool fromILDJIT)
{
  //  cerr << tid << "push" << endl;
  checkFirstAccess(tid);
  ///  ReleaseLock(&simbuffer_lock);
  PIN_Yield();
  GetLock(locks_[tid], tid+1);
  reserveHandshake(tid, fromILDJIT);  

  if((didFirstInsn_.count(tid) == 0) && (!fromILDJIT)) {    
    handshake->flags.isFirstInsn = true;
    didFirstInsn_[tid] = true;
  }
  else {
    handshake->flags.isFirstInsn = false;
  }

  if(produceBuffer_[tid]->size() > 0) {
    assert(produceBuffer_[tid]->back()->flags.valid == true);
  }

  if(produceBuffer_[tid]->full()) {
    int produceSize = produceBuffer_[tid]->size();
    copyProducerToFile(tid);
    assert(fileEntryCount_[tid] > 0);
    assert(fileEntryCount_[tid] >= produceSize);
    assert(produceBuffer_[tid]->size() == 0);
  }
 
  assert(!produceBuffer_[tid]->full());

  produceBuffer_[tid]->push(handshake);
  queueSizes_[tid]++;  
  pool_[tid]--;

  ReleaseLock(locks_[tid]);
  PIN_Yield();
  //  GetLock(&simbuffer_lock, tid+1);
  //  cerr << tid << "push end" << endl;
}

void BufferManager::pop(THREADID tid, handshake_container_t* handshake)
{
  checkFirstAccess(tid);
  GetLock(locks_[tid], tid+1);

  assert(queueSizes_[tid] > 0);
  assert(consumeBuffer_[tid]->size() > 0);
  consumeBuffer_[tid]->pop();

  pool_[tid]++;
  queueSizes_[tid]--;
  ReleaseLock(locks_[tid]);
}

bool BufferManager::hasThread(THREADID tid) 
{
  bool result = queueSizes_.count(tid);
  return (result != 0);
}

unsigned int BufferManager::size()
{
  unsigned int result = queueSizes_.size();
  return result;
}

void BufferManager::checkFirstAccess(THREADID tid)
{
  if(queueSizes_.count(tid) == 0) {
    
    PIN_Yield();
    sleep(5);
    PIN_Yield();
    
    queueSizes_[tid] = 0;
    fileEntryCount_[tid] = 0;
    consumeBuffer_[tid] = new Buffer();
    fakeFile_[tid] = new Buffer();
    produceBuffer_[tid] = new Buffer();    
    pool_[tid] = 100000;

    cerr << tid << " Allocating locks!" << endl;
    locks_[tid] = new PIN_LOCK();
    InitLock(locks_[tid]);
    
    char s_tid[100];
    sprintf(s_tid, "%d", tid);

    string logName = "./output_ring_cache/handshake_" + string(s_tid) + ".log"; 
    logs_[tid] = new ofstream();
    (*(logs_[tid])).open(logName.c_str());    
 

    fileNames_[tid] = tempnam("/dev/shm/", "A");
    bogusNames_[tid] = tempnam("/dev/shm/", "B");

    cerr << "Created " << fileNames_[tid] << " and " << bogusNames_[tid] << endl;

    int fd = open(fileNames_[tid].c_str(), O_WRONLY | O_CREAT, 0777);
    int result = close(fd);
    if(result == -1) {
      cerr << "Close error: " << " Errcode:" << strerror(errno) << endl;  
      abort();
    }
    sync(); 
  } 

  GetLock(locks_[tid], tid+1);
  if(useRealFile_) {
    assert((consumeBuffer_[tid]->size() + produceBuffer_[tid]->size() + fileEntryCount_[tid]) == queueSizes_[tid]);
  }
  else {
    assert((consumeBuffer_[tid]->size() + produceBuffer_[tid]->size() + fakeFile_[tid]->size()) == queueSizes_[tid]);
  }
  ReleaseLock(locks_[tid]);
}

void BufferManager::reserveHandshake(THREADID tid, bool fromILDJIT)
{
  //  checkFirstAccess(tid);    
  
  long long int spins = 0;  
  bool somethingConsumed = false;
  int lastSize = queueSizes_[tid];

  while(pool_[tid] == 0) {    
    ReleaseLock(locks_[tid]);
    //    ReleaseLock(&simbuffer_lock);
    PIN_Yield();
    //    GetLock(&simbuffer_lock, tid+1);
    GetLock(locks_[tid], tid+1);
    spins++;
    int newSize = queueSizes_[tid];
    assert(newSize <= lastSize);
    somethingConsumed = (newSize != lastSize);
    lastSize = newSize;

    if(spins >= 70000000LL) {
      assert(queueSizes_[tid] > 0);
      pool_[tid] += queueSizes_[tid];
      cerr << tid << " [reserveHandshake()]: Increasing file up to " << queueSizes_[tid] + pool_[tid] << endl;
      break;
    }
    if(somethingConsumed) {      
      spins = 0;
    }
  }
}

bool BufferManager::isFirstInsn(THREADID tid)
{
  GetLock(locks_[tid], tid+1);
  bool isFirst = (didFirstInsn_.count(tid) == 0);
  ReleaseLock(locks_[tid]);
  return isFirst;
}

void BufferManager::copyProducerToFile(THREADID tid)
{
  if(useRealFile_) {
    copyProducerToFileReal(tid);
  }
  else {
    assert(false);
    copyProducerToFileFake(tid);
  }
}

void BufferManager::copyFileToConsumer(THREADID tid)
{
  if(useRealFile_) {
    copyFileToConsumerReal(tid);
  }
  else {
    assert(false);
    copyFileToConsumerFake(tid);
  }
}

void BufferManager::copyProducerToFileFake(THREADID tid)
{
  while(produceBuffer_[tid]->size() > 0) {
    if(fakeFile_[tid]->full()) {      
      break;
    }

    if(produceBuffer_[tid]->front()->flags.valid == false) {
      break;
    }

    fakeFile_[tid]->push(produceBuffer_[tid]->front());
    produceBuffer_[tid]->pop();
  }
}


void BufferManager::copyFileToConsumerFake(THREADID tid)
{
  while(fakeFile_[tid]->size() > 0) {
    if(consumeBuffer_[tid]->full()) {      
      break;
    }

    assert(fakeFile_[tid]->front()->flags.valid);

    consumeBuffer_[tid]->push(fakeFile_[tid]->front());
    fakeFile_[tid]->pop();
  }
}


void BufferManager::copyProducerToFileReal(THREADID tid)
{
  int result;

  int fd = open(fileNames_[tid].c_str(), O_WRONLY | O_APPEND);
  if(fd == -1) {
    cerr << "Opened to write: " << fileNames_[tid].c_str();
    cerr << "Pipe open error: " << fd << " Errcode:" << strerror(errno) << endl;
    abort();
  }

  int count = 0;
  while(produceBuffer_[tid]->size() > 0) {
    if(produceBuffer_[tid]->front()->flags.valid == false) {
      assert(produceBuffer_[tid]->size() == 1);
      break;
    }
    
    writeHandshake(fd, produceBuffer_[tid]->front());
    produceBuffer_[tid]->pop();
    count++;
    fileEntryCount_[tid]++;
  }

  result = close(fd);
  if(result == -1) {
    cerr << "Close error: " << " Errcode:" << strerror(errno) << endl;  
    abort();
  }

  sync();

  assert(produceBuffer_[tid]->size() == 1 || produceBuffer_[tid]->size() == 0);
  assert(fileEntryCount_[tid] >= 0);
}


void BufferManager::copyFileToConsumerReal(THREADID tid)
{
  int result;
  
  int fd = open(fileNames_[tid].c_str(), O_RDONLY);
  if(fd == -1) {
    cerr << "Opened to read: " << fileNames_[tid].c_str();
    cerr << "Pipe open error: " << fd << " Errcode:" << strerror(errno) << endl;
    abort();
  }
  
  int fd_bogus = open(bogusNames_[tid].c_str(), O_WRONLY | O_CREAT, 0777);
  if(fd_bogus == -1) {
    cerr << "Opened to write: " << bogusNames_[tid].c_str();
    cerr << "Pipe open error: " << fd_bogus << " Errcode:" << strerror(errno) << endl;
    abort();
  }
  
  int count = 0;
  bool validRead = true;
  while(fileEntryCount_[tid] > 0) {
    handshake_container_t handshake;
    if(consumeBuffer_[tid]->full()) {      
      break;
    }
    validRead = readHandshake(fd, &handshake);    
    assert(validRead);
    consumeBuffer_[tid]->push(&(handshake));
    count++;
    fileEntryCount_[tid]--;
  }
  
  
  int copyCount = 0;
  while(validRead) {
    handshake_container_t handshake;
    validRead = readHandshake(fd, &handshake);
    if(!validRead) {
      break;
    }
    writeHandshake(fd_bogus, &handshake);
    copyCount++;
  }
  
  {
    handshake_container_t handshake;
    assert(!readHandshake(fd, &handshake));
    struct stat buf;
    fstat(fd_bogus, &buf);
    int size = buf.st_size;
    assert(consumeBuffer_[tid]->full() || (copyCount == 0 && (size == 0)));
 }

  result = close(fd_bogus);
  if(result == -1) {
    cerr << "Close error: " << " Errcode:" << strerror(errno) << endl;  
    abort();
  }
  result = close(fd);
  if(result == -1) {
    cerr << "Close error: " << " Errcode:" << strerror(errno) << endl;  
    abort();
  }

  sync();
  
  result = rename(bogusNames_[tid].c_str(), fileNames_[tid].c_str());
  if(result == -1) {
    cerr << "Can't rename filesystem bridge files. " << " Errcode:" << strerror(errno) << endl;
    abort();
  }
  
  sync();

  assert(fileEntryCount_[tid] >= 0);
}

void BufferManager::writeHandshake(int fd, handshake_container_t* handshake)
{
  int mapSize = handshake->mem_buffer.size();    
  const int handshakeBytes = sizeof(P2Z_HANDSHAKE);
  const int flagBytes = sizeof(handshake_flags_t);
  const int mapEntryBytes = sizeof(UINT32) + sizeof(UINT8);
  int mapBytes = mapSize * mapEntryBytes;
  int totalBytes = sizeof(int) + handshakeBytes + flagBytes + mapBytes;
  
  void * writeBuffer = (void*)malloc(totalBytes);
  void * buffPosition = writeBuffer;

  memcpy((char*)buffPosition, &(mapSize), sizeof(int));
  buffPosition = (char*)buffPosition + sizeof(int);

  memcpy((char*)buffPosition, &(handshake->handshake), handshakeBytes);
  buffPosition = (char*)buffPosition + handshakeBytes;

  memcpy((char*)buffPosition, &(handshake->flags), flagBytes);
  buffPosition = (char*)buffPosition + flagBytes;
  
  map<UINT32, UINT8>::iterator it;
  for(it = handshake->mem_buffer.begin(); it != handshake->mem_buffer.end(); it++) {
    memcpy((char*)buffPosition, &(it->first), sizeof(UINT32));
    buffPosition = (char*)buffPosition + sizeof(UINT32);
    
    memcpy((char*)buffPosition, &(it->second), sizeof(UINT8));
    buffPosition = (char*)buffPosition + sizeof(UINT8);
  }
  
  assert(((unsigned long long int)writeBuffer) + totalBytes == ((unsigned long long int)buffPosition));
  
  int bytesWritten = write(fd, writeBuffer, totalBytes);
  if(bytesWritten == -1) {
    cerr << "Pipe write error: " << bytesWritten << " Errcode:" << strerror(errno) << endl;
    abort();
  }
  if(bytesWritten != totalBytes) {
    cerr << "File write error: " << bytesWritten << " expected:" << totalBytes << endl;
    abort();
  }  
  free(writeBuffer);
}

bool BufferManager::readHandshake(int fd, handshake_container_t* handshake)
{
  const int handshakeBytes = sizeof(P2Z_HANDSHAKE);
  const int flagBytes = sizeof(handshake_flags_t);
  const int mapEntryBytes = sizeof(UINT32) + sizeof(UINT8);
  
  int mapSize;
  int bytesRead = read(fd, &(mapSize), sizeof(int));
  if(bytesRead == 0) {
    return false;
  }
  if(bytesRead == -1) {
    cerr << "File read error: " << bytesRead << " Errcode:" << strerror(errno) << endl;
    abort();
  }
  assert(bytesRead == sizeof(int));    
    
  int mapBytes = mapSize * mapEntryBytes;
  int totalBytes = handshakeBytes + flagBytes + mapBytes;

  void * readBuffer = (void*)malloc(totalBytes);

  bytesRead = read(fd, readBuffer, totalBytes);
  if(bytesRead == -1) {
    cerr << "File read error: " << bytesRead << " Errcode:" << strerror(errno) << endl;    
    abort();
  }  
  assert(bytesRead == totalBytes);    
    
  void * buffPosition = readBuffer;
  memcpy(&(handshake->handshake), buffPosition, handshakeBytes);
  buffPosition = (char*)buffPosition + handshakeBytes;  
  
  memcpy(&(handshake->flags), buffPosition, flagBytes);
  buffPosition = (char*)buffPosition + flagBytes;
  
  handshake->mem_buffer.clear();
  for(int i = 0; i < mapSize; i++) {
    UINT32 first;
    UINT8 second;
    
    first = *((UINT32*)buffPosition);
    buffPosition = (char*)buffPosition + sizeof(UINT32);
    
    second = *((UINT8*)buffPosition);
    buffPosition = (char*)buffPosition + sizeof(UINT8);
    
    (handshake->mem_buffer)[first] = second;
  }
  
  assert(((unsigned long long int)readBuffer) + totalBytes == ((unsigned long long int)buffPosition));
  
  free(readBuffer);

  return true;
}

void BufferManager::signalCallback(int signum)
{
  cerr << "BufferManager caught signal:" << signum << endl;
  map<THREADID, string>::iterator it;
  for(it = fileNames_.begin(); it != fileNames_.end(); it++) {
    string cmd = "rm -rf " + fileNames_[it->first] + " " + bogusNames_[it->first];
    system(cmd.c_str());
  }
  exit(1);
}
