#include  <ctype.h>
#include  <stdio.h>
#include  <stdlib.h>
#include  <stdarg.h>
#include  <stdbool.h>
#include  <time.h>
#include  <unistd.h>
#include  <inttypes.h>
#include  <sys/stat.h>
#include  <sys/types.h>
#include  <sys/wait.h>
#include  <errno.h>
#include  <string.h>
#include  <dirent.h>
#include  <fcntl.h>

#define GLOBAL_BUFFER_CAP 1024 * 10

#define LOG_TYPES  3
#define PREFIX_LEN 24
#define BUFFER_LEN 256
#define SUFFIX_LEN 6

#ifndef LOG_WARN_ENABLED
#define LOG_WARN_ENABLED 1
#endif

#ifndef LOG_INFO_ENABLED
#define LOG_INFO_ENABLED  1
#endif

#define NOB_ERROR(fmt, ...) nobLog(LOG_ERROR, fmt, ##__VA_ARGS__)

#if LOG_WARN_ENABLED == 1
#define NOB_WARN(fmt, ...)  nobLog(LOG_WARN , fmt, ##__VA_ARGS__)
#else
#define NOB_WARN(fmt, ...)
#endif

#if LOG_INFO_ENABLED == 1
#define NOB_INFO(fmt, ...)  nobLog(LOG_INFO , fmt, ##__VA_ARGS__) 
#else
#define NOB_INFO(fmt, ...)
#endif

#define NOB_ASSERT(X)                                                                                   \
do{                                                                                                     \
  if(!(X)) nobLog(LOG_ERROR, "%s  func : %s   file : %s line : %d", #X, __FUNCTION__, __FILE__, __LINE__);\
}while(0);                                                                                              \

#define INITCLOCK   struct timespec start; clock_gettime(CLOCK_MONOTONIC, &start) 
#define WATCH(X) elapsed(&start, X)

#define BUFF_CHECK(result, limit)                    \
if(result >= limit){                                  \
  fprintf(stderr, "result size exceeded limit\n");    \
  return result;                                      \
}else if( result < 0){                                \
  fprintf(stderr, "error in %s\n", __FUNCTION__);     \
  return result;                                      \
}

#define RETURN_DEFER(value) do { result = (value); goto defer; } while(0)

typedef uint64_t  u64;
typedef int64_t   i64;

typedef float     f32;
typedef double    f64;

// darray of pointers
typedef struct  NobStrings  NobStrings;

// darray of charachters 
typedef struct  NobString   NobString;

// just a pointer and the size of string
typedef struct  NobStringView NobStringView;

// static array (owner of a non resizeable string);
typedef struct  NobBuffer   NobBuffer;

struct NobBuffer{
  const char* const items;
  const u64         cap;
  u64               count;
};

struct  NobStringView{
  char*   items;
  u64     count;
};

struct  NobString{
  char*   items;
  u64     cap;
  u64     count;
};

struct  NobStrings{
  char**  items;
  u64     cap;
  u64     count;
};

typedef enum LOG_LEVEL {
  LOG_INFO  = 0,
  LOG_WARN  = 1,
  LOG_ERROR = 2,
} LOG_LEVEL;

typedef enum {
  NOB_FILE_REGULAR = 0,
  NOB_FILE_DIRECTORY,
  NOB_FILE_SYMLINK,
  NOB_FILE_OTHER,
} NOB_FILE_TYPE;

typedef enum GLOB_RESULT{
  GLOB_UNMATCHED  = 0,
  GLOB_MATCHED    = 1,
  GLOB_SYNTAX_ERROR=2,
}GLOB_RESULT;

static NobBuffer* globalBuffer;

static const char* logLevels[LOG_TYPES]  = 
  { "󰆤 ❯ ",
    " ❯ ",
    "󰯆 ❯ ",
  };

static const char* logColors[LOG_TYPES + 1] =
  {
    "\x1b[94m",
    "\x1b[93m",
    "\x1b[91m",
    "\x1b[0m\n"
  };
// custom allocation operations
void*         nobAlloc(u64 count, u64 stride);
void*         nobRealloc(void* original, u64  newsize);

// global buffer operations
void          nobInit();
char*         nobGlobalBufferAlloc(u64  size);
char*         nobGlobalBufferStrdup(const char* string, u64 size);

// logging operations
u64           nobLog(LOG_LEVEL level , const char* message , ...);
u64           nobWrite(u64 limit, const char *message, ...) ;

// filesystem operations
NOB_FILE_TYPE nobGetFileType(const char* path);
NobBuffer*    nobReadFile   (const char* path);
bool          nobIsDir      (const char* path);
bool          nobMkdir      (const char* path);
bool          nobCopyFile   (const char* src, const char* dst);
bool          nobReadDir    (const char* parent, NobStrings* children);

// parsing operations
NobStrings*   nobTokenize   (NobBuffer* buffer);
char*         nobChopByDelim(NobStringView* sv, char delim);
char*         nobChopBySpace(NobStringView* sv);
GLOB_RESULT   matchGlob(const char* pattern, const char* text);
bool          globe(const char* pattern, const char* text);
char*         replace(char* string, char* substr, char* replace);

// process oerations
bool proc_wait(int p);
bool cmd_run_sync  (NobStrings* commands);
int  cmd_run_async (NobStrings* commands);

void elapsed(struct timespec* start, const char* func){
  struct timespec end;
  clock_gettime(CLOCK_MONOTONIC, &end);

  double elapsed = (end.tv_sec - start->tv_sec) + (end.tv_nsec - start->tv_nsec)/1e9;
  NOB_INFO("%-10s : %.9f",func,elapsed);
  *start  = end;
}

u64   nobLog(LOG_LEVEL level , const char* message , ...){
  char outmsg[PREFIX_LEN + BUFFER_LEN + SUFFIX_LEN];
  u64   prefixlen = snprintf  (outmsg,
                               PREFIX_LEN,"%s%s%1s",logColors[level], logLevels[level],"");
  BUFF_CHECK(prefixlen, PREFIX_LEN);
  va_list args;
  va_start(args , message);
  u64   bufferlen    = vsnprintf (outmsg + prefixlen,
                                  BUFFER_LEN, message , args);
  va_end(args);
  BUFF_CHECK(bufferlen, BUFFER_LEN);
  u64   suffixlen    = snprintf  (outmsg + prefixlen + bufferlen,
                                  SUFFIX_LEN, "%s", logColors[LOG_TYPES]);
  BUFF_CHECK(suffixlen, SUFFIX_LEN);
  u64 totallen = prefixlen + bufferlen + suffixlen;
  if(write(STDOUT_FILENO, outmsg,totallen ) < 0){
    fprintf(stderr, "ULog failed with %s\n", strerror(errno));
  }

  return bufferlen ;
}

u64   nobWrite(u64 limit, const char *message, ...) {
  va_list args;
  va_start(args, message);
  char buffer[limit];
  u64 size  = vsnprintf(buffer,limit, message, args);
  BUFF_CHECK(size, limit);
  va_end(args);

  if(write(STDOUT_FILENO, buffer, size) < 0){
    fprintf(stderr," nobWrite FAILED WITH %s\n", strerror(errno));
    return 0;
  }; 
  return size;
}

void nobInit(){
  globalBuffer  = nobAlloc(1, sizeof(NobBuffer));
  NobBuffer temp  = {
    .cap    = GLOBAL_BUFFER_CAP,
    .items  = nobAlloc(GLOBAL_BUFFER_CAP, sizeof(char)),
    .count  = 0
  };
  memcpy(globalBuffer,&temp, sizeof(NobBuffer));
}

void* nobAlloc(u64 count, u64 stride){
  void* mem = calloc(count, stride);
  if(mem  ==  NULL){
    NOB_ERROR("LINE : %d  | %s", __LINE__, strerror(errno));
    exit(1);
  }
  return mem;
}

void* nobRealloc(void* original, u64  newsize){
  void* mem = realloc(original , newsize);
  if(mem  == NULL){
    NOB_ERROR("LINE : %d  | %s", __LINE__, strerror(errno));
    exit(1);
  }
  return mem;
}


char* nobGlobalBufferAlloc(u64  size){
  if(globalBuffer->count + size >= globalBuffer->cap){
    NOB_ERROR("MAX BUFFER MEMMORY ALREADY USED !");
    exit(1);
  }
  const char* handle  = globalBuffer->items + globalBuffer->count;
  globalBuffer->count +=  size;
  return (char*)handle;
}

char* nobGlobalBufferStrdup(const char* string, u64 size){
  char* handle  = nobGlobalBufferAlloc(size + 1);
  strncpy(handle, string, size);
  handle[size]  = '\0';
  return handle;
}

#define DA_INIT_CAP 1

#define DA_APPEND(DA, ITEM)                                                       \
do{                                                                               \
  if((DA)->count >= (DA)->cap || (DA)->cap == 0)                                  \
  {                                                                               \
    (DA)->cap     = (DA)->cap == 0 ? DA_INIT_CAP : (DA)->cap * 2;                 \
    (DA)->items   = nobRealloc((DA)->items, (DA)->cap * sizeof(*(DA)->items));    \
  }                                                                               \
  (DA)->items[(DA)->count++] = ITEM;                                              \
}while(0);                                                                        


#define DA_APPEND_MANY(DA, NEW_ITEMS, COUNT)                                      \
do{                                                                               \
  if((DA)->count + (COUNT) >= (DA)->cap)                                          \
  {                                                                               \
    if((DA)->count  ==  0){(DA)->cap  = DA_INIT_CAP;}                             \
    while((DA)->count + (COUNT) > (DA)->cap){(DA)->cap *= 2;}                     \
    (DA)->items = nobRealloc((DA)->items, (DA)->cap * sizeof(*(DA)->items));      \
  }                                                                               \
  memcpy((DA)->items + (DA)->count, (NEW_ITEMS), (COUNT) * sizeof(*(DA)->items)); \
  (DA)->count +=  (COUNT);                                                        \
}while(0)                                                                         

#define DA_RESERVE(DA,  COUNT)                                                    \
do{                                                                               \
  if((DA)->count  + (COUNT) > (DA)->cap){                                         \
    if((DA)->count  ==  0){(DA)->cap  = DA_INIT_CAP;}                             \
    while((DA)->count + (COUNT) > (DA)->cap){(DA)->cap *= 2;}                     \
    (DA)->items = nobRealloc((DA)->items, (DA)->cap * sizeof(*(DA)->items));      \
  }                                                                               \
}while(0)                                                             


#define DA_REMOVE(DA, COUNT)                                                                                    \
do{                                                                                                             \
if     ((COUNT) < 1)     {NOB_WARN("%"PRIu64"(invalid) items being popped",(COUNT));}                            \
else if((DA)->count < (COUNT)){NOB_WARN("cannot remove %"PRIu64" items from %s, %"PRIu64" items are avialable",  \
                                        (COUNT),#DA,(DA)->count);}                                              \
else if((DA)->count < 1) {NOB_WARN("%s is already empty", #DA);}                                                 \
else{ memset((DA)->items + (DA)->count - (COUNT) , 0, (COUNT) * sizeof(*(DA)->items));(DA)->count -= (COUNT);}\
}while(0)                                                                                                       \

#define DA_EMPTY(DA)  \
do{                   \
  memset((DA)->items, 0, (DA)->count * sizeof(*(DA)->items));\
  (DA)->count = 0;    \
}while(0);            \

#define DA_DEBUG(DA)                                                                          \
do{                                                                                           \
  NOB_INFO("object  : %s  | cap  : %"PRIu64"  | count : %"PRIu64"",#DA, (DA)->cap, (DA)->count); \
}while(0);                                                                                    \

NOB_FILE_TYPE nobGetFileType(const char* path){
  struct stat stats;
  if(stat(path, &stats) < 0){
    NOB_ERROR("COULD NOT GET STATS OF %s : %s",path, strerror(errno));
    return -1;
  }

  switch(stats.st_mode & S_IFMT){
    case  S_IFDIR : return NOB_FILE_DIRECTORY;
    case  S_IFREG : return NOB_FILE_REGULAR;
    case  S_IFLNK : return NOB_FILE_SYMLINK;
    default       : return NOB_FILE_OTHER;
  }
}

bool nobIsDir(const char* path){
  struct stat stats;
  stat(path , &stats);

  return S_ISDIR(stats.st_mode)? true : false;
}

bool nobMkdir(const char* path){
  bool exists = nobIsDir(path);
  if(exists){
    //NOB_INFO("'%s' ALREAD EXISTS",path);
    return true;
  }else{
    NOB_WARN("PATH NOT EXIST , MAKING '%s'",path);
    if(mkdir(path, S_IFDIR) < 0){
      NOB_ERROR("COULD NOT MAKE %s  : %s",path,strerror(errno));
      return false;
    }else{
      NOB_INFO("CREATED '%s'",path);
      return true;
    }
  }
}

void printStrings(NobStrings* fp){
  for(int i = 0; i < fp->count; ++i){
    NOB_INFO("%s", fp->items[i]);
  }
}

bool nobReadDir(const char* parent, NobStrings* children){
  bool result = true;
  DIR* dir  = {0};

  dir = opendir(parent);
  if(dir  ==   NULL){
    NOB_ERROR("COULD NOT OPEN %s : %s",parent, strerror(errno));
    RETURN_DEFER(false);
  }  
  errno = 0;
  struct dirent* entry  = readdir(dir);
  while(entry != NULL){

    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      entry = readdir(dir);
      continue;
    }

    DA_APPEND(children,nobGlobalBufferStrdup(entry->d_name,strlen(entry->d_name)));
    entry = readdir(dir);
  }

  if(errno != 0){
    NOB_ERROR("COULD NOT READ DIR %s  : %s",parent, strerror(errno));
    RETURN_DEFER(false);
  }
defer:
  if(dir) closedir(dir);
  return false;
}

bool  nobCopyFile (const char* srcPath, const char* dstPath){
  i64 srcfd  = -1;
  i64 dstfd  = -1;
  u64 bufsize= 32 * 1024;
  char* buf = nobRealloc(NULL, bufsize);
  NOB_ASSERT(buf != NULL);
  bool result = true;

  srcfd  = open(srcPath, O_RDONLY);
  if(srcfd < 0){
    NOB_ERROR("COULD NOT OPEN FILE %s : %s",srcPath, strerror(errno));
    RETURN_DEFER(false);
  }

  struct stat srcstat;
  if(fstat(srcfd,&srcstat) < 0){
    NOB_ERROR("COULD NOT GET STATS OF FILE  %s  : %s",srcPath,strerror(errno));
  }

  dstfd = open(dstPath, O_CREAT | O_TRUNC | O_WRONLY, srcstat.st_mode);
  if(dstfd < 0){
    NOB_ERROR("COULD NOT CREATE FILE %s : %s", dstPath, strerror(errno));
  }

  for(;;){
    u64 n = read(srcfd, buf, bufsize);
    if(n == 0) break;
    if(n < 0){
      NOB_ERROR("COULD NOT READ FROM FILE  %s  : %s",srcPath,strerror(errno));
      RETURN_DEFER(false);
    }
    char* buf2  = buf;
    while(n > 0){
      u64 m = write(dstfd, buf2, n);
      if(m < 0){
        NOB_ERROR("COULD NOT WRITE TO FILE  %s  : %s",dstPath, strerror(errno));
      }

      n -=  m;
      buf2  +=  m;
    }
  }

defer:
  free(buf);
  close(srcfd);
  close(dstfd);

  return result;
}

NobBuffer*  nobReadFile(const char* path){
  bool result = true;
  FILE* file  = fopen(path, "r");
  if(file == NULL)                  RETURN_DEFER(false);
  if(fseek(file, 0, SEEK_END) < 0)  RETURN_DEFER(false);
  i64 filesize  = ftell(file);
  NOB_INFO("filesize : %"PRIu64"",filesize);
  if(filesize < 0)                  RETURN_DEFER(false);
  if(fseek(file, 0, SEEK_SET) < 0)  RETURN_DEFER(false);

  char* mem = nobGlobalBufferAlloc(filesize + 1);

  i64 bytesread = fread(mem, sizeof(char), filesize, file);
  NOB_INFO("bytesread : %"PRIu64"",bytesread);
  if(ferror(file))                  RETURN_DEFER(false);
  if(bytesread < filesize)          RETURN_DEFER(false);
  mem[bytesread] = '\0';

  NobBuffer temp  = {.count = bytesread + 1, .cap = bytesread + 1, .items = mem};
  NobBuffer*  filebuffer  = nobAlloc(1, sizeof(NobBuffer));
  memcpy(filebuffer, &temp, sizeof(NobBuffer));

defer:
  if(!result){
    NOB_ERROR("FAILED TO OPEN FILE %s : %s", path, strerror(errno));
    if(file) fclose(file);
    exit(1);
  }  
  if(file) fclose(file);
  return filebuffer;
}

//----------------------------------------STRING OPERATIONS-----------------------------------
char* nobChopByDelim(NobStringView* sv, char delim){
  u64 i = 0;
  while(i < sv->count && sv->items[i] != delim){
    ++i;
  }
  char* chopped = nobGlobalBufferStrdup(sv->items, i);
  if(i < sv->count){
    sv->items += i + 1;
    sv->count -= i + 1;
  }else{
    sv->items +=  i;
    sv->count -=  i;
  }
  return chopped;
}

char* nobChopBySpace(NobStringView* sv){
  u64 i = 0;

  while(i < sv->count && !isspace(sv->items[i])){
    ++i;
  }
  char* chopped = nobGlobalBufferStrdup(sv->items, i);
  if(i < sv->count){
    sv->items += (i + 1);
    sv->count -= (i + 1);
  }else{
    sv->items +=  i;
    sv->count -=  i;
  }
  return chopped;
}


NobStrings* nobTokenize( NobBuffer* buffer) {
  NobStringView sv  = {0};
  sv.items  = (char*)buffer->items;
  sv.count  = buffer->count;
  NobStrings* tokens = nobAlloc(1, sizeof(NobStrings));

  while(sv.count > 0){
    char* token = nobChopBySpace(&sv);
    if (token[0] != '\0') {
      DA_APPEND(tokens, token);
    }
  } 
  return tokens;
}

//----------------------------------------------------GLOB---------------------------------------
GLOB_RESULT matchGlob(const char* pattern, const char* text){

  while(*pattern  != '\0' && *text  != '\0'){
    NOB_INFO("%c  --> %c", *pattern, *text); 
    switch (*pattern) {

      case '?':{
        ++pattern;
        ++text;
      }break;


      case '*':{
        GLOB_RESULT result  = matchGlob(pattern+1, text);
        if(result){
          return GLOB_MATCHED;
        }else{
          ++text;
        }
      }break;

      //[][!]
      case '[':{
        ++pattern;//skipping first
        bool matched  = GLOB_UNMATCHED;
        bool negated  = false;

        if(*pattern == '\0'){NOB_WARN("END OF LINE AFTER '[' "); return GLOB_SYNTAX_ERROR;}
        //negate 
        if(*pattern == '!'){
          negated = true;
          ++pattern;
        }

        if(*pattern == '\0'){NOB_WARN("END OF LINE AFTER '[' "); return GLOB_SYNTAX_ERROR;}
        //first and prev char
        char prev = *pattern;
        matched |= prev == *text;
        pattern +=1;

        while(*pattern != ']' && *pattern != '\0'){
          switch (*pattern) {
            case '-'  :{
              ++pattern;
              switch(*pattern){
                case '\0':{
                  return GLOB_SYNTAX_ERROR;
                }break;
                case ']':{
                  matched |= '-' == *text;
                }break;
                default:{
                  matched |= prev <= *text && *text <= *pattern;
                  prev = *pattern;
                  ++pattern;
                }
              }
            }break;
            default   :{
              prev = *pattern;
              matched |= prev == *text;
              ++pattern;
            } 
          }
        }

        if((*pattern != ']')){NOB_WARN("INCOMPLETE '[' "); return GLOB_SYNTAX_ERROR;}
        if(negated) matched = !matched;
        if(GLOB_UNMATCHED) return GLOB_UNMATCHED;

        ++pattern;
        ++text;
      }break;


      case '\\' :{
        ++pattern;
        if(*pattern == '\0'){NOB_WARN("INCOMPLETE '\'"); return GLOB_SYNTAX_ERROR;}
      }__attribute__ ((fallthrough));//GNU STATEMENT ATTRIBUTES
      //FALLTHROUGH
      default:{
        if(*pattern == * text){
          ++pattern;
          ++text;
        }else{
          NOB_WARN("'%c' not matched in delfault case", *pattern);
          return GLOB_UNMATCHED;
        }
      }

    }//while
  }
  if(*text == '\0'){
    while (*pattern == '*') ++pattern;
    return *pattern == '\0';
  }
  return GLOB_UNMATCHED;
}


bool  globe(const char* pattern, const char* text){
  GLOB_RESULT result = matchGlob(pattern, text);
  switch (result) {
    case GLOB_MATCHED:{
      NOB_INFO("%-10s --> %-10s  : MATCHED",  pattern , text);
      return true;
    }break;
    case GLOB_UNMATCHED:{
      NOB_WARN("%-10s --> %-10s  : FAILED",   pattern , text);
      return false;
    }break;
    case GLOB_SYNTAX_ERROR:{
      NOB_ERROR("%-10s --> %-10s  : SYNTAX ERROR",   pattern , text);    
      return false;
    }
  }
}

char* replace(char* string, char* substr, char* replace){
  char* substrbegin = strstr(string, substr);
  NobString final = {0};
  int iter = 0;
  int reducedlen  = substrbegin - string;
  int replacelen  = strlen(replace);
  DA_APPEND_MANY(&final, string, reducedlen); 
  DA_APPEND_MANY(&final, replace, replacelen);
  char* replaced  = nobGlobalBufferStrdup(final.items, final.count);
  free(final.items);
  return replaced;
}

#define SHADER_TYPES_NUM 2

typedef enum{
  SHADER_VERT = 0,
  SHADER_FRAG,
}SHADER_TYPE;

static  char* compiler  = "glslc";
static  char* shadertypes[SHADER_TYPES_NUM]  = {
  "-fshader-stage=vert",
  "-fshader-stage=frag"
};


void compile(SHADER_TYPE type, NobStrings fileNames, NobStrings targetNames){

  NOB_INFO("COMPILING %s",shadertypes[type]);
  NobStrings commands = {0};

  NOB_INFO("NUMBER OF FILES : %d",fileNames.count);
    DA_APPEND(&commands, compiler);
    DA_APPEND(&commands, shadertypes[type]);
  for(u64 i = 0; i< fileNames.count; ++i){
    DA_APPEND(&commands, fileNames.items[i]);
    DA_APPEND(&commands, "-o");
    DA_APPEND(&commands, targetNames.items[i]);
    cmd_run_sync(&commands);
    DA_REMOVE(&commands, 4);
  }
}

#define INVALID_PROC -1



int cmd_run_async(NobStrings* commands){
  NobString args  = {0};
  for(u64 i = 0; i < commands->count; ++i){
    u64 len = strlen(commands->items[i]);
    DA_APPEND_MANY(&args, commands->items[i], len);
    DA_APPEND(&args, ' ');
  }
  DA_APPEND(commands, NULL);

 // NOB_INFO("FOOD : %s", args.items);

  pid_t cpid = fork();

  if (cpid < 0) {
    NOB_ERROR("COULD NOT FORK CHILD PROCESS: %s", strerror(errno));
    return INVALID_PROC;
  } 

  if (cpid > 0) {
    // This block is executed by the parent process
    NOB_INFO("AWAKENED CHILD No. %d", cpid);
  }

  if (cpid == 0) {
    // This block is executed by the child process
    NOB_INFO("CHILD EATING FLAGS");
    //child exits after succesfull or unsuccesful exec of execvp
    if (execvp(commands->items[0], (char* const*)commands->items) < 0) {
      NOB_ERROR("CHILD COULD NOT EAT FLAGS: %s", strerror(errno));
      exit(1);
    }
  }


  return cpid;
}



bool proc_wait(int p){
  for(;;){
    int wstatus = 0;
    if(waitpid(p, &wstatus, 0) < 0){
      NOB_ERROR("COULD NOT WAIT ON [PID %d] : %s",p, strerror(errno));
      return false;
    }
    if(WIFEXITED(wstatus)){
      int exit_status = WEXITSTATUS(wstatus);
      if(exit_status < 0){
        NOB_ERROR("PROCESS EXITED WITH CODE %d", exit_status);
      }
      break;
    }
    if(WIFSIGNALED(wstatus)){
      NOB_ERROR("PROCESS TERMINATED BY SIGNAL %s",strsignal(WTERMSIG(wstatus)));
      return false;
    }
  }

  return true;
}


bool cmd_run_sync(NobStrings* commands){

  INITCLOCK;
  int p = cmd_run_async(commands);
  if(p == INVALID_PROC) return false;

  bool b = proc_wait(p);
  WATCH("FLAGS FINISHED IN ");
  return b;
}


int main(void){

  char* src_path  = "assets/shaders/src/";
  char* dst_path  = "assets/shaders/bin/";
  nobInit();
  if(nobIsDir(src_path)){
    NOB_INFO("SOURCE  : '%s'",src_path);
  }else{
    NOB_ERROR("SOURCE : '%4s' NOT FOUND",src_path);
    exit(1);
  }
  if(nobMkdir(dst_path)){
    NOB_INFO("DEST    : '%4s'",dst_path);
  }else{
    exit(1);
  }
  // NOB_INFO("--------------READING CONFIG FILE--------------");
  // NobBuffer* configBuffer = nobReadFile("shader.nob");
  // nobWrite(configBuffer->cap ,"%s",configBuffer->items);
  NOB_INFO("--------------READING SOURCE FOLDER-------------");
  NobStrings   files  = {0};
  nobReadDir(src_path,&files);
  printStrings(&files);
  NOB_INFO("--------------IDENTIFYING TARGETS--------------");
  NobStrings fragList = {0};
  NobStrings targetFragList = {0};  
  NobStrings vertList = {0};
  NobStrings targetVertList = {0};
  char* filetype  = "glsl";
  char* targetfiletype  = "spv";
  int   filetypelen = strlen(filetype);
  int   targetfiletypelen = strlen(targetfiletype);
  int srclen  = strlen(src_path);
  int dstlen  = strlen(dst_path);
  int filenamelen = 0;
  for( u64 i = 0 ;i < files.count; ++i){
    if(strstr(files.items[i], "frag.glsl") != NULL){
      NobString file  = {0};
      filenamelen = strlen(files.items[i]);
      DA_APPEND_MANY(&file, src_path, srclen);
      DA_APPEND_MANY(&file, files.items[i], filenamelen);
      DA_APPEND(&fragList, nobGlobalBufferStrdup(file.items, file.count));
      DA_EMPTY(&file);
      DA_APPEND_MANY(&file, dst_path, dstlen);
      DA_APPEND_MANY(&file, files.items[i], filenamelen - filetypelen);
      DA_APPEND_MANY(&file, targetfiletype, targetfiletypelen);
      DA_APPEND(&targetFragList, nobGlobalBufferStrdup(file.items, file.count));
      free(file.items);
    }else if(strstr(files.items[i], "vert.glsl") != NULL){
      NobString file  = {0};
      filenamelen = strlen(files.items[i]);
      DA_APPEND_MANY(&file, src_path, srclen);
      DA_APPEND_MANY(&file, files.items[i], filenamelen);
      DA_APPEND(&vertList, nobGlobalBufferStrdup(file.items, file.count));
      DA_EMPTY(&file);
      DA_APPEND_MANY(&file, dst_path, dstlen);
      DA_APPEND_MANY(&file, files.items[i], filenamelen - filetypelen);
      DA_APPEND_MANY(&file, targetfiletype, targetfiletypelen);
      DA_APPEND(&targetVertList, nobGlobalBufferStrdup(file.items, file.count));
      free(file.items);
    }
  }

  printStrings(&fragList);
  printStrings(&vertList);

  compile(SHADER_FRAG, fragList, targetFragList);
  compile(SHADER_VERT, vertList, targetVertList);

  DA_EMPTY(&files)
  nobReadDir(src_path,&files);

  return 0;
}
