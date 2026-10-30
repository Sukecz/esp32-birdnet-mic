#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

// Only the SD worker may call this class. A journal failure never changes WAV status.
class SdJournal {
    int fd = -1;
    uint32_t limit = 1048576, count = 0;
    bool dirty = false;
    char path[64] = {}, backup[68] = {};
    int failure = 0;
    bool fail() { failure=errno?errno:EIO; if(fd>=0)::close(fd);fd=-1;dirty=false;return false; }
    bool create() { fd=::open(path,O_WRONLY|O_CREAT|O_APPEND,0666);return fd>=0 || fail(); }
    bool rotate() {
        if(!flush())return false;
        if(fd>=0 && ::close(fd)) {fd=-1;return fail();}fd=-1;
        struct stat info;
        if(!::stat(backup,&info)) {
            if(!S_ISREG(info.st_mode)){errno=EISDIR;return fail();}
            if(::unlink(backup))return fail();
        }else if(errno!=ENOENT)return fail();
        if(::rename(path,backup))return fail();
        count=0;return create();
    }
public:
    bool begin(const char* file,uint32_t maxBytes) {
        close();failure=0;limit=maxBytes;count=0;
        if(strlen(file)>=sizeof(path)){errno=ENAMETOOLONG;return fail();}
        strcpy(path,file);snprintf(backup,sizeof(backup),"%s.1",file);
        struct stat info;
        if(!::stat(path,&info)) {
            if(!S_ISREG(info.st_mode)){errno=EISDIR;return fail();}
            if(info.st_size<0 || uint64_t(info.st_size)>UINT32_MAX){errno=EFBIG;return fail();}
            count=uint32_t(info.st_size);
        }else if(errno!=ENOENT)return fail();
        return create();
    }
    bool append(const char* text) {
        if(fd<0)return false;
        const size_t n=strlen(text);
        if(n+1>limit){errno=EFBIG;return fail();}
        if(count>limit || count+n+1>limit)if(!rotate())return false;
        const ssize_t wrote=::write(fd,text,n);
        if(wrote!=ssize_t(n))return fail();
        if(::write(fd,"\n",1)!=1)return fail();
        count+=uint32_t(n+1);dirty=true;return true;
    }
    bool flush() {if(fd<0)return false;if(dirty && ::fsync(fd))return fail();dirty=false;return true;}
    bool close() {
        if(fd<0)return !failure;
        if(!flush())return false;
        const int result=::close(fd);fd=-1;if(result)return fail();return true;
    }
    bool active() const{return fd>=0;}
    uint32_t size() const{return count;}
    int errorCode() const{return failure;}
};
