/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Pixelw and contributors
 * See android/LICENSE for the full license text.
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "ipc_channel.h"
#include "smi_file.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int owner=-1,writer_dir=-1,writer=-1,reader_dir=-1,reader=-1;
static struct shm_structure *write_map;
static const struct shm_structure *read_map;
static pthread_mutex_t read_lock=PTHREAD_MUTEX_INITIALIZER;

int lx_ipc_writer_start(int reset){
  if(owner>=0){errno=EALREADY;return -1;}
  writer_dir=lx_smi_open_directory(LX_IPC_DIRECTORY);
  if(writer_dir<0)return -1;
  owner=openat(writer_dir,"owner.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
  struct stat st;
  if(owner<0 || fstat(owner,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() ||
     (st.st_mode&0777)!=0600 || st.st_nlink!=1 || st.st_size!=0 || flock(owner,LOCK_EX|LOCK_NB)){
    int saved=errno?errno:EACCES;lx_ipc_writer_finish();errno=saved;return -1;
  }
  int old=lx_smi_open_reader(writer_dir);
  if(old>=0){
    if(!reset){close(old);lx_ipc_writer_finish();errno=EEXIST;return -1;}
    int current=lx_smi_is_current(writer_dir,old);
    close(old);
    if(!current || unlinkat(writer_dir,LX_SMI_LEAF,0)){lx_ipc_writer_finish();errno=EACCES;return -1;}
  }else if(errno!=ENOENT){int saved=errno;lx_ipc_writer_finish();errno=saved;return -1;}
  return 0;
}
int lx_ipc_writer_create(void){
  if(writer_dir<0 || owner<0){errno=EINVAL;return -1;}
  if(writer>=0)return writer;
  writer=lx_smi_create(writer_dir);
  if(writer<0)return -1;
  write_map=mmap(NULL,sizeof(*write_map),PROT_READ|PROT_WRITE,MAP_SHARED,writer,0);
  if(write_map==MAP_FAILED){write_map=NULL;lx_ipc_writer_drop();return -1;}
  struct shm_structure zero={.version=NQPTP_SHM_STRUCTURES_VERSION};
  if(lx_smi_publish(writer,write_map,&zero)){lx_ipc_writer_drop();return -1;}
  return writer;
}
int lx_ipc_writer_publish(const struct shm_structure *value){
  if(writer<0 || !write_map || !lx_smi_is_current(writer_dir,writer)){errno=ESTALE;return -1;}
  return lx_smi_publish(writer,write_map,value);
}
int lx_ipc_writer_drop(void){
  int result=0;
  if(writer>=0){
    if(lx_smi_is_current(writer_dir,writer) && unlinkat(writer_dir,LX_SMI_LEAF,0))result=-1;
    if(write_map)munmap(write_map,sizeof(*write_map));
    close(writer);writer=-1;write_map=NULL;
  }
  return result;
}
void lx_ipc_writer_finish(void){
  (void)lx_ipc_writer_drop();
  if(owner>=0)close(owner);
  if(writer_dir>=0)close(writer_dir);
  owner=writer_dir=-1;
}
static int reader_owner=-1,held_generation=-1,reader_active=1;
static dev_t previous_dev;
static ino_t previous_ino;
static int owner_valid(int fd){
  struct stat st;
  if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() ||
     (st.st_mode&0777)!=0600 || st.st_nlink!=1 || st.st_size!=0){errno=EACCES;return 0;}
  return 1;
}
static int writer_alive(void){
  struct stat opened,named;
  if(reader_owner<0 || !owner_valid(reader_owner) || fstat(reader_owner,&opened) ||
     fstatat(reader_dir,"owner.lock",&named,AT_SYMLINK_NOFOLLOW) || opened.st_dev!=named.st_dev || opened.st_ino!=named.st_ino){errno=ESTALE;return 0;}
  if(flock(reader_owner,LOCK_SH|LOCK_NB)==0){flock(reader_owner,LOCK_UN);errno=EPIPE;return 0;}
  return errno==EAGAIN || errno==EWOULDBLOCK;
}
static int reader_close_locked(void){
  int result=0;
  if(read_map)result=munmap((void *)read_map,sizeof(*read_map));
  if(reader>=0)close(reader);
  if(reader_owner>=0)close(reader_owner);
  if(reader_dir>=0)close(reader_dir);
  read_map=NULL;reader=reader_dir=reader_owner=-1;
  return result;
}
static int reader_open_locked(void){
  if(reader>=0 && lx_smi_is_current(reader_dir,reader) && writer_alive())return 0;
  reader_close_locked();
  reader_dir=lx_smi_open_directory(LX_IPC_DIRECTORY);
  if(reader_dir<0)goto fail;
  reader_owner=openat(reader_dir,"owner.lock",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
  if(reader_owner<0 || !owner_valid(reader_owner) || !writer_alive())goto fail;
  reader=lx_smi_open_reader(reader_dir);
  if(reader<0)goto fail;
  if(held_generation>=0){
    struct stat st;if(fstat(reader,&st))goto fail;
    if(st.st_dev==previous_dev && st.st_ino==previous_ino){errno=ENOENT;goto fail;}
    close(held_generation);held_generation=-1;
  }
  read_map=mmap(NULL,sizeof(*read_map),PROT_READ,MAP_SHARED,reader,0);
  if(read_map==MAP_FAILED){read_map=NULL;goto fail;}
  return 0;
fail:{int saved=errno;reader_close_locked();errno=saved;return -1;}
}
int lx_ipc_reader_open(void){pthread_mutex_lock(&read_lock);int result=reader_open_locked();pthread_mutex_unlock(&read_lock);return result;}
void lx_ipc_reader_begin(void){
  pthread_mutex_lock(&read_lock);
  if(held_generation>=0)close(held_generation);
  held_generation=-1;
  int directory=lx_smi_open_directory(LX_IPC_DIRECTORY);
  if(directory>=0){
    held_generation=lx_smi_open_reader(directory);
    if(held_generation>=0){struct stat st;if(!fstat(held_generation,&st)){previous_dev=st.st_dev;previous_ino=st.st_ino;}else{close(held_generation);held_generation=-1;}}
    close(directory);
  }
  reader_close_locked();reader_active=1;
  pthread_mutex_unlock(&read_lock);
}
void lx_ipc_reader_pause(void){pthread_mutex_lock(&read_lock);reader_active=0;reader_close_locked();pthread_mutex_unlock(&read_lock);}
int lx_ipc_reader_snapshot(struct shm_structure *value,uint64_t now){
  pthread_mutex_lock(&read_lock);
  int result=-1;
  if(!reader_active){errno=EAGAIN;goto done;}
  if(reader_open_locked() || !writer_alive())goto done;
  result=lx_smi_snapshot(reader,read_map,value);
  if(!result && value->main.master_clock_id){
    /* RAW 时效、未来点和非法 master 起点必须拒绝；不是声学同步证明。 */
    if(!value->main.local_time || value->main.local_time>now || now-value->main.local_time>2000000000ULL ||
       value->main.master_clock_start_time>value->main.local_time){errno=EAGAIN;result=-1;}
  }
done:pthread_mutex_unlock(&read_lock);return result;
}
int lx_ipc_reader_close(void){
  pthread_mutex_lock(&read_lock);int result=reader_close_locked();
  if(held_generation>=0)close(held_generation);
  held_generation=-1;reader_active=1;
  pthread_mutex_unlock(&read_lock);return result;
}
