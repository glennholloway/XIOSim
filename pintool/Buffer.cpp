#include "Buffer.h"

Buffer::Buffer()
{
  numPool_ = 50000;
  handshakePool_ = new handshake_container_t* [numPool_];

  for (int i = 0; i < numPool_; i++) {
    handshake_container_t* newHandshake = new handshake_container_t();
    handshakePool_[i] = newHandshake;      
  }
  head_ = 0;
  tail_ = 0;
  size_ = 0;
}

handshake_container_t* Buffer::get_buffer()
{
  assert(!full());
  return handshakePool_[head_];
}

void Buffer::push_done()
{
  assert(!full());

  head_++;
  if(head_ == numPool_) {
    head_ = 0;
  }
  size_++;
}

void Buffer::pop()
{
  handshake_container_t* handshake = front();
  handshake->clear();

  tail_++;
  if(tail_ == numPool_) {
    tail_ = 0;
  }
  size_--;
}

handshake_container_t* Buffer::front()
{
  assert(size_ > 0);
  return handshakePool_[tail_];
}

handshake_container_t* Buffer::back()
{
  assert(size_ > 0);
  int dex = (head_ - 1);
  if(dex == -1 ) {
    dex = numPool_ - 1;
  }
  return handshakePool_[dex];
}

bool Buffer::empty()
{
  return size_ == 0;
}

bool Buffer::full()
{
  return size_ == numPool_;
}

int Buffer::size()
{
  return size_;
}
