/* Autore: Raffaele Ariano 530519
 * Il programma e', in ogni sua parte, opera originale dell'autore*/

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <errno.h>

#include <connections.h>
#include <message.h>
#include <conn.h>

int openConnection(char* path, unsigned int ntimes, unsigned int secs){
  if(path==NULL)
    return -1;
  struct sockaddr_un serv_addr;
  int socketFD;
  socketFD=socket(AF_UNIX, SOCK_STREAM, 0);
  if(socketFD == -1){
    perror("socket");
    return -1;
  }
  memset(&serv_addr, '0', sizeof(serv_addr));
  serv_addr.sun_family = AF_UNIX;
  strncpy(serv_addr.sun_path,path, strlen(path)+1);
  int i;
  int connessione;
  for(i=0; i<ntimes; i++){//provo a connettermi
    connessione=connect(socketFD,(const struct sockaddr *)&serv_addr,sizeof(serv_addr));
    if(connessione>=0){
      return socketFD;
    }
    else
      sleep(secs);
  }
  return -1; //se non mi connetto entro ntimes volte allora ritorno -1
}

int readHeader(long connfd, message_hdr_t *hdr){
  if(hdr==NULL)
    return -1;
  int readder=readn(connfd,hdr,sizeof(message_hdr_t));
  return readder;
}

int readData(long fd, message_data_t *data){
  if(data==NULL)
    return -1;
  int readata=readn(fd,&data->hdr,sizeof(message_data_hdr_t));
  if(readata<0)
    return -1;
  if(data->hdr.len==0)
    data->buf=NULL;
  else{
    data->buf=malloc(sizeof(char)*data->hdr.len);
    memset(data->buf,0,sizeof(char)*data->hdr.len);
    readata=readn(fd,data->buf,data->hdr.len);
  }
  return readata;
}


  
int readMsg(long fd, message_t *msg){
  if(msg==NULL){
    return -1;
  }
  int readmess=readHeader(fd,&msg->hdr);
  if(readmess<0)
    return readmess;
  readmess=readData(fd,&msg->data);
  return readmess;
}

int sendRequest(long fd, message_t *msg){
  if(msg==NULL)
    return -1;
  int rit=sendHeader(fd,&msg->hdr);
  if(rit<0)
    return rit;
  rit=sendData(fd,&msg->data);
  return rit;
}

int sendData(long fd, message_data_t *msg){
  if(msg==NULL)
    return -1;
  int ritorn=writen(fd,&msg->hdr,sizeof(message_data_hdr_t));
  if(ritorn<0)
    return -1;
  ritorn=writen(fd,msg->buf,msg->hdr.len);
  return ritorn;
}

int sendHeader(long fd, message_hdr_t * hdr){
  if(hdr==NULL)
    return -1;
  return (writen(fd,hdr,sizeof(message_hdr_t)));
}