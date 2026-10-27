#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_REQ 65536
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static sqlite3 *db;
static int version = 3;
static const char *json_path = "data/tasks.json";
static const char *db_path = "data/tasks.db";

static char *read_file(const char *path) {
 FILE *f=fopen(path,"rb"); if(!f)return NULL;
 if(fseek(f,0,SEEK_END)||ftell(f)>MAX_REQ){fclose(f);return NULL;}
 long n=ftell(f);rewind(f);char *s=calloc((size_t)n+1,1);
 if(!s){fclose(f);return NULL;}if(fread(s,1,(size_t)n,f)!=(size_t)n){free(s);fclose(f);return NULL;}fclose(f);return s;
}
static int write_json(json_object *arr) {
 char tmp[256];snprintf(tmp,sizeof(tmp),"%s.tmp",json_path);
 FILE *f=fopen(tmp,"wb");if(!f)return -1;
 const char *s=json_object_to_json_string_ext(arr,JSON_C_TO_STRING_PRETTY);
 int ok=fputs(s,f)>=0 && fflush(f)==0 && fsync(fileno(f))==0 && fclose(f)==0;
 if(!ok){remove(tmp);return -1;}return rename(tmp,json_path);
}
static json_object *load_json(void) {
 char *s=read_file(json_path);json_object *a=s?json_tokener_parse(s):NULL;free(s);
 if(!a||!json_object_is_type(a,json_type_array)){if(a)json_object_put(a);return json_object_new_array();}return a;
}
static int migrate(void){
 if(sqlite3_exec(db,"CREATE TABLE IF NOT EXISTS tasks (id INTEGER PRIMARY KEY, title TEXT NOT NULL, completed INTEGER NOT NULL DEFAULT 0 CHECK(completed IN (0,1)))",NULL,NULL,NULL)!=SQLITE_OK)return -1;
 sqlite3_stmt *cnt=NULL;int count=0;
 if(sqlite3_prepare_v2(db,"SELECT COUNT(*) FROM tasks",-1,&cnt,NULL)!=SQLITE_OK)return -1;
 if(sqlite3_step(cnt)==SQLITE_ROW)count=sqlite3_column_int(cnt,0);sqlite3_finalize(cnt);
 if(count)return 0;
 char *raw=read_file(json_path);if(!raw)return 0;
 json_object *arr=json_tokener_parse(raw);free(raw);
 if(!arr||!json_object_is_type(arr,json_type_array)){if(arr)json_object_put(arr);return -1;}
 if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK){json_object_put(arr);return -1;}
 sqlite3_stmt *stmt=NULL;int ok=sqlite3_prepare_v2(db,"INSERT OR IGNORE INTO tasks(id,title,completed) VALUES(?,?,?)",-1,&stmt,NULL)==SQLITE_OK;
 for(int i=0;ok&&i<json_object_array_length(arr);i++){
  json_object *o=json_object_array_get_idx(arr,i),*v=NULL;
  if(!json_object_object_get_ex(o,"id",&v))continue;int id=json_object_get_int(v);
  if(!json_object_object_get_ex(o,"title",&v))continue;const char *title=json_object_get_string(v);
  int done=json_object_object_get_ex(o,"completed",&v)?json_object_get_boolean(v):0;
  if(id<=0||!title||!*title)continue;
  sqlite3_bind_int(stmt,1,id);sqlite3_bind_text(stmt,2,title,-1,SQLITE_TRANSIENT);sqlite3_bind_int(stmt,3,done);
  ok=sqlite3_step(stmt)==SQLITE_DONE;sqlite3_reset(stmt);sqlite3_clear_bindings(stmt);
 }
 sqlite3_finalize(stmt);json_object_put(arr);
 if(sqlite3_exec(db,ok?"COMMIT":"ROLLBACK",NULL,NULL,NULL)!=SQLITE_OK)return -1;
 return ok?0:-1;
}
static json_object *list_tasks(void){
 if(version<3)return load_json();
 json_object *a=json_object_new_array();sqlite3_stmt *s=NULL;
 if(sqlite3_prepare_v2(db,"SELECT id,title,completed FROM tasks ORDER BY id",-1,&s,NULL)!=SQLITE_OK)return a;
 while(sqlite3_step(s)==SQLITE_ROW){json_object *o=json_object_new_object();
 json_object_object_add(o,"id",json_object_new_int(sqlite3_column_int(s,0)));
 json_object_object_add(o,"title",json_object_new_string((const char*)sqlite3_column_text(s,1)));
 json_object_object_add(o,"completed",json_object_new_boolean(sqlite3_column_int(s,2)));
 json_object_array_add(a,o);}
 sqlite3_finalize(s);return a;
}
static int create_task(const char *title){
 if(version<3){json_object *a=load_json();int id=0;
 for(int i=0;i<json_object_array_length(a);i++){json_object *v=NULL,*o=json_object_array_get_idx(a,i);if(json_object_object_get_ex(o,"id",&v)&&json_object_get_int(v)>id)id=json_object_get_int(v);}
 json_object *o=json_object_new_object();json_object_object_add(o,"id",json_object_new_int(++id));json_object_object_add(o,"title",json_object_new_string(title));json_object_object_add(o,"completed",json_object_new_boolean(0));json_object_array_add(a,o);int ok=write_json(a);json_object_put(a);return ok==0?id:-1;}
 sqlite3_stmt *s=NULL;int id=-1;
 if(sqlite3_prepare_v2(db,"INSERT INTO tasks(title,completed) VALUES(?,0)",-1,&s,NULL)==SQLITE_OK){sqlite3_bind_text(s,1,title,-1,SQLITE_TRANSIENT);if(sqlite3_step(s)==SQLITE_DONE)id=(int)sqlite3_last_insert_rowid(db);}sqlite3_finalize(s);return id;
}
static int modify_task(int id,int completed,int remove_it){
 if(version<3){json_object *a=load_json(),*out=json_object_new_array();int found=0;
 for(int i=0;i<json_object_array_length(a);i++){json_object *o=json_object_array_get_idx(a,i),*v=NULL;int oid=json_object_object_get_ex(o,"id",&v)?json_object_get_int(v):-1;
 if(oid==id){found=1;if(remove_it)continue;json_object_object_add(o,"completed",json_object_new_boolean(completed));}
 json_object_array_add(out,json_object_get(o));}
 int ok=found?write_json(out):0;json_object_put(out);json_object_put(a);return ok?-1:found;}
 sqlite3_stmt *s=NULL;const char *sql=remove_it?"DELETE FROM tasks WHERE id=?":"UPDATE tasks SET completed=? WHERE id=?";
 if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK)return -1;
 if(!remove_it)sqlite3_bind_int(s,1,completed);sqlite3_bind_int(s,remove_it?1:2,id);
 int ok=sqlite3_step(s)==SQLITE_DONE;int changes=sqlite3_changes(db);sqlite3_finalize(s);return ok?!!changes:-1;
}
static int send_all(int fd,const char *p,size_t n){while(n){ssize_t k=send(fd,p,n,0);if(k<=0)return -1;p+=k;n-=k;}return 0;}
static void reply(int fd,int status,const char *type,const char *body){
 const char *msg=status==200?"OK":status==201?"Created":status==204?"No Content":status==400?"Bad Request":status==404?"Not Found":status==405?"Method Not Allowed":"Internal Server Error";
 char h[512];size_t len=strlen(body);int n=snprintf(h,sizeof(h),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",status,msg,type,len);send_all(fd,h,(size_t)n);send_all(fd,body,len);
}
static void json_reply(int fd,int status,json_object *o){reply(fd,status,"application/json; charset=utf-8",json_object_to_json_string(o));json_object_put(o);}
static void error_reply(int fd,int status,const char *message){json_object *o=json_object_new_object();json_object_object_add(o,"error",json_object_new_string(message));json_reply(fd,status,o);}
static void handle(int fd){
 char buf[MAX_REQ+1];size_t have=0;ssize_t k;char *head=NULL;
 while(have<MAX_REQ && (k=recv(fd,buf+have,MAX_REQ-have,0))>0){have+=(size_t)k;buf[have]=0;head=strstr(buf,"\r\n\r\n");if(head)break;}
 if(!head){error_reply(fd,400,"Invalid HTTP request");return;}
 size_t header_len=(size_t)(head+4-buf);char method[16]={0},path[256]={0};
 if(sscanf(buf,"%15s %255s",method,path)!=2){error_reply(fd,400,"Invalid request line");return;}
 int content_length=0;char *p=strstr(buf,"Content-Length:");if(!p)p=strstr(buf,"content-length:");if(p)content_length=atoi(strchr(p,':')+1);
 if(content_length<0||header_len+(size_t)content_length>MAX_REQ){error_reply(fd,400,"Body too large");return;}
 while(have<header_len+(size_t)content_length){k=recv(fd,buf+have,MAX_REQ-have,0);if(k<=0)break;have+=(size_t)k;}
 if(have<header_len+(size_t)content_length){error_reply(fd,400,"Incomplete request");return;}
 buf[header_len+(size_t)content_length]=0;
 if(strcmp(path,"/api/version")==0&&strcmp(method,"GET")==0){char body[64];snprintf(body,sizeof(body),"{\"version\":%d}",version);reply(fd,200,"application/json",body);return;}
 if(strcmp(path,"/api/tasks")==0&&strcmp(method,"GET")==0){pthread_mutex_lock(&lock);json_object *a=list_tasks();pthread_mutex_unlock(&lock);json_reply(fd,200,a);return;}
 if(strcmp(path,"/api/tasks")==0&&strcmp(method,"POST")==0){
 json_object *o=json_tokener_parse(buf+header_len),*v=NULL;
 if(!o||!json_object_object_get_ex(o,"title",&v)||!json_object_is_type(v,json_type_string)){if(o)json_object_put(o);error_reply(fd,400,"title is required");return;}
 const char *title=json_object_get_string(v);if(!title||!strlen(title)||strlen(title)>200){json_object_put(o);error_reply(fd,400,"title must be 1..200 characters");return;}
 pthread_mutex_lock(&lock);int id=create_task(title);pthread_mutex_unlock(&lock);json_object_put(o);
 if(id<0){error_reply(fd,500,"Could not create task");return;}json_object *res=json_object_new_object();json_object_object_add(res,"id",json_object_new_int(id));json_reply(fd,201,res);return;}
 int id=0;char extra=0;
 if(sscanf(path,"/api/tasks/%d%c",&id,&extra)==1&&id>0){
 if(strcmp(method,"DELETE")==0){pthread_mutex_lock(&lock);int ok=modify_task(id,0,1);pthread_mutex_unlock(&lock);if(ok==1)reply(fd,204,"application/json","");else error_reply(fd,ok==0?404:500,"Task not found or storage error");return;}
 if(strcmp(method,"PATCH")==0){json_object *o=json_tokener_parse(buf+header_len),*v=NULL;
 if(!o||!json_object_object_get_ex(o,"completed",&v)||!json_object_is_type(v,json_type_boolean)){if(o)json_object_put(o);error_reply(fd,400,"completed must be boolean");return;}
 int completed=json_object_get_boolean(v);json_object_put(o);pthread_mutex_lock(&lock);int ok=modify_task(id,completed,0);pthread_mutex_unlock(&lock);
 if(ok==1)reply(fd,200,"application/json","{\"ok\":true}");else error_reply(fd,ok==0?404:500,"Task not found or storage error");return;}}
 if(strcmp(method,"GET")==0){const char *file=NULL,*type="text/plain";
 if(strcmp(path,"/")==0||strcmp(path,"/index.html")==0){file="public/index.html";type="text/html; charset=utf-8";}
 else if(strcmp(path,"/style.css")==0){file="public/style.css";type="text/css";}
 else if(strcmp(path,"/app.js")==0){file="public/app.js";type="application/javascript";}
 if(file){char *s=read_file(file);if(s){reply(fd,200,type,s);free(s);}else error_reply(fd,404,"File missing");return;}}
 error_reply(fd,404,"Not found");
}
static void *client_thread(void *arg){int fd=*(int*)arg;free(arg);struct timeval tv={5,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));handle(fd);close(fd);return NULL;}
int main(int argc,char **argv){
 int port=8080;for(int i=1;i<argc;i++){if(strcmp(argv[i],"--version")==0&&i+1<argc)version=atoi(argv[++i]);else if(strcmp(argv[i],"--port")==0&&i+1<argc)port=atoi(argv[++i]);else {fprintf(stderr,"Usage: %s [--version 1|2|3] [--port 8080]\n",argv[0]);return 1;}}
 if(version<1||version>3||port<1||port>65535){fprintf(stderr,"Invalid version or port\n");return 1;}
 mkdir("data",0755);if(version==3){if(sqlite3_open(db_path,&db)!=SQLITE_OK||migrate()!=0){fprintf(stderr,"SQLite initialization/migration failed\n");return 1;}}
 signal(SIGPIPE,SIG_IGN);int s=socket(AF_INET,SOCK_STREAM,0);int reuse=1;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
 struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons((uint16_t)port),.sin_addr.s_addr=htonl(INADDR_ANY)};
 if(bind(s,(struct sockaddr*)&addr,sizeof(addr))<0||listen(s,32)<0){perror("bind/listen");return 1;}
 printf("Task Manager V%d running at http://localhost:%d (Ctrl+C to stop)\n",version,port);fflush(stdout);
 for(;;){int fd=accept(s,NULL,NULL);if(fd<0)continue;int *arg=malloc(sizeof(int));if(!arg){close(fd);continue;}*arg=fd;pthread_t t;if(pthread_create(&t,NULL,client_thread,arg)==0)pthread_detach(t);else{free(arg);close(fd);}}
}
