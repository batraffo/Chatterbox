/*
 * Autore: Raffaele Ariano 530519
 * Il programma e', in ogni sua parte, opera originale dell'autore
 *
 *
 * membox Progetto del corso di LSO 2017
 *
 * Dipartimento di Informatica Università di Pisa
 * Docenti: Prencipe, Torquati
 * 
 */
/**
 * @file chatty.c
 * @brief File principale del server chatterbox
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>

#include <assert.h>
#include <icl_hash.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>   
#include <sys/select.h>
#include <stats.h>
#include <conn.h>
#include <connections.h>
#include <icl_hash.h>
#include <limits.h>
#include "parsing.h"

/* inserire gli altri include che servono */
#define SockName "chatterboxsock_530519"
#define BITS_IN_int     ( sizeof(int) * CHAR_BIT )
#define THREE_QUARTERS  ((int) ((BITS_IN_int * 3) / 4))
#define ONE_EIGHTH      ((int) (BITS_IN_int / 8))
#define HIGH_BITS       ( ~((unsigned int)(~0) >> ONE_EIGHTH ))

struct statistics  chattyStats = { 0,0,0,0,0,0,0 };

int MaxFileSize; /*!< variabile parsata da chatty.conf */
int MaxConnections; /*!< variabile parsata da chatty.conf */
int ThreadsInPool; /*!< variabile parsata da chatty.conf */
int MaxMsgSize; /*!< variabile parsata da chatty.conf */
int MaxHistMsgs; /*!< variabile parsata da chatty.conf */
char * StatFileName; /*!< variabile parsata da chatty.conf */
char *DirName; /*!< variabile parsata da chatty.conf */
char * UnixPath; /*!< variabile parsata da chatty.conf */
FILE *stat; /*!<file in cui stampero' le statistiche */

int numutenticonnessi;
fd_set set; 
int fdmax;

/**
 *
 *@brief Lista di utenti che appartengono ad un gruppo.
 *      
 *
 *Abbiamo un puntatore a questa struttura per ogni elemento della tabellagruppi.
 *
 */

typedef struct elem_group{
	char* utente;
	struct elem_group* next;
}gruppo;

/**
 *@brief Lista di gruppi.
 *
 *La struttura tiene traccia di diversi gruppi (lista di utenti) e dei loro nomi.
 *
 */

typedef struct tebellagruppi
{
	char * nomegruppo;
	gruppo * gruppo;
	struct tebellagruppi* next;
}tabgroup;

/**
 *@brief la struttura piu' importante: la lista delle operazioni da eseguire!
 *
 *Questa lista e' utilizzata per tenere traccia di tutte le operazioni da eseguire e contiene
 *l'indice connessione dell'utente che richiede l'operazione e il messaggio che esso invia.
 */

typedef struct _elem { 
   int connfd; 
   struct _elem *next;
   message_t messaggio;
} coda;

/**
 *Semplicemente la struttura che utilizzero' per tenere traccia degli utenti connessi.
 *
 */

typedef struct connessi{
	char utente[MAX_NAME_LENGTH];
	int connfd;
	struct connessi *next;
}listaconn;

static listaconn * listaconnessi;
icl_hash_t * hash; /*!< la tabella hash degli utenti registrati */
static coda * codino; /*!< la coda con le operazioni da eseguire */
static tabgroup ** tabellagruppi; /*!< servira' per creare un array di puntatori alle liste di gruppi, l'array verra' gestito come una tabella hash */
static pthread_cond_t codavuota=PTHREAD_COND_INITIALIZER; /*!< se la coda operazioni e' vuota questa sara' la condizione che fara' rilasciare la lock */
static pthread_mutex_t mutecscodaoperazioni=PTHREAD_MUTEX_INITIALIZER; /*!< lock per gestire l'uso concorrente della coda delle operazioni   */
static pthread_mutex_t mutecslistaconn=PTHREAD_MUTEX_INITIALIZER; /*!< lock per gestire l'uso concorrente della coda degli utenti connessi   */
static pthread_mutex_t mutecsfmax=PTHREAD_MUTEX_INITIALIZER; /*!< lock per gestire l'uso concorrente di fdmax */
static pthread_mutex_t mutecstats=PTHREAD_MUTEX_INITIALIZER; /*!< lock per gestire l'uso concorrente  delle statistiche   */
static pthread_mutex_t *locker_gruppi; /*!< array di lock per gestire l'uso concorrente della tabella hash dei gruppi    */
static pthread_mutex_t *locker_fd; /*!< lock per gestire la concorrenza per quel che riguarda l'invio e la  lettura di messaggi di un utente connesso   */


volatile sig_atomic_t siguscita_flag;/*!< variabile sigatomica per uscire dal ciclo infinito della select e della funzione "lavora" eseguita da ogni thread  */

/**
 * Gestore dei segnali
 *
 */

static void gestore (int signum) {
	if(signum==SIGTERM || signum==SIGQUIT){
		siguscita_flag=0;
	}
	if (signum==SIGUSR1){
		pthread_mutex_lock(&mutecslistaconn);
		pthread_mutex_lock(&mutecstats);
		chattyStats.nonline=numutenticonnessi;
		printStats(stat);
		pthread_mutex_unlock(&mutecstats);
		pthread_mutex_unlock(&mutecslistaconn);
	}
}

/**
 * Serve per aggiornare fdmax
 */

int updatemax(fd_set set, int fdma) {
    for(int i=(fdma-1);i>=0;--i)
	if (FD_ISSET(i, &set)) return i;
    return -1;
}

/**
 * Funzione che uso per liberare la memoria della parte dati di ogni elemento della tabella hash
 */
 
void datafreer(void * tofree){
	mess_t * messaggio=(mess_t *) tofree;
	mess_t * curr;
	if(messaggio->mess!=NULL){
		while(messaggio!=NULL){
			curr=messaggio;
			free(curr->mess);
			curr->mess=NULL;
			free(curr->sender);
			curr->sender=NULL;
			messaggio=messaggio->next;
			free(curr);
			curr=NULL;
		}
	}
	else{
		free(messaggio);
		messaggio=NULL;
	}

}

int isconnesso(char * utente){
	if(listaconnessi==NULL)
		return 0;
	listaconn * current=listaconnessi;
	while(current!=NULL){
		if (strcmp(current->utente,utente)==0)
			return current->connfd;
		else
			current=current->next;
	}
	return 0;
}

/**
 * Funzione che utilizzo per generare, a partire dal nome del gruppo, la sua posizione in tabellagruppi
 *
 */

int funzionecasuale(char * unnomeacaso){
	int n=0;
	for(int i=0; i<strlen(unnomeacaso);i++)
		n=n+(int)unnomeacaso[i];
	n=n*strlen(unnomeacaso);
	n=n%256;
	return n;
}

/**
 * Creo un nuovo e lo inserisco nella tabellagruppi
 *
 */

int inesertgruppo(char* nomegruppo, char *primoutente){
	int risultatofunzione=funzionecasuale(nomegruppo);
	tabgroup * curr;
	int k=0;
	pthread_mutex_lock(&locker_gruppi[risultatofunzione % 128]);
	if(tabellagruppi[risultatofunzione]==NULL){
		tabellagruppi[risultatofunzione]=malloc(sizeof(tabgroup));
		k=1;
	}
	curr=tabellagruppi[risultatofunzione];
	tabgroup *quellodiprima=NULL;
	if(k!=1){
		while(curr!=NULL){
			if(strcmp(nomegruppo,curr->nomegruppo)==0){
				pthread_mutex_unlock(&locker_gruppi[risultatofunzione % 128]);
				return -1;
			}
			else{
				quellodiprima=curr;
				curr=curr->next;	
			}
		}
	}
	curr->nomegruppo=malloc(sizeof(char)*33);
	strcpy(curr->nomegruppo,nomegruppo);
	gruppo * gruppo=malloc(sizeof(gruppo));
	gruppo->next=NULL;
	gruppo->utente=malloc(33*sizeof(char));
	strcpy(gruppo->utente,primoutente);
	curr->gruppo=gruppo;
	curr->next=NULL;
	if(quellodiprima!=NULL)
		quellodiprima->next=curr;
	pthread_mutex_unlock(&locker_gruppi[risultatofunzione % 128]);
	return strlen(nomegruppo);
}

/**
 * Inserisco l'utente "nomeutente" nel gruppo "nomegruppo"
 *
 */

int searchandinsert(char * nomeutente, char* nomegruppo){
	int risultatofunzione=funzionecasuale(nomegruppo);
	tabgroup * curr;
	gruppo * currente;
	pthread_mutex_lock(&locker_gruppi[risultatofunzione % 128]);
	curr=tabellagruppi[risultatofunzione];
	while(curr!=NULL){
		if(strcmp(curr->nomegruppo,nomegruppo)!=0)
			curr=curr->next;
		else{
			currente=curr->gruppo;
			if(currente==NULL){
				curr->gruppo=malloc(sizeof(gruppo));
				curr->gruppo->next=NULL;
				curr->gruppo->utente=malloc((strlen(nomeutente)+1)*sizeof(char));
				strcpy(curr->gruppo->utente,nomeutente);
				pthread_mutex_unlock(&locker_gruppi[risultatofunzione % 128]);
				return strlen(nomeutente);
			}
			while(currente->next!=NULL){
				if(strcmp(currente->utente,nomeutente)==0){
					pthread_mutex_unlock(&locker_gruppi[risultatofunzione % 128]);
					return -1;
				}
				else
					currente=currente->next;
			}
			if(strcmp(currente->utente,nomeutente)==0){
				pthread_mutex_unlock(&locker_gruppi[risultatofunzione % 128]);
				return -1;
			}
			else{
				gruppo * nuovo=malloc(sizeof(gruppo));
				nuovo->utente=malloc(33*sizeof(char));
				strcpy(nuovo->utente,nomeutente);
				nuovo->next=NULL;
				currente->next=nuovo;
				pthread_mutex_unlock(&locker_gruppi[risultatofunzione % 128]);
				return strlen(nomeutente);
			}
		}
	}
	pthread_mutex_unlock(&locker_gruppi[risultatofunzione % 128]);
	return -1;
}

/**
 * Elimino l'utente "nomeutente" dal gruppo "nomegruppo"
 *
 */

int searchanddelete(char * nomeutente, char* nomegruppo){
	int risultatofunzione=funzionecasuale(nomegruppo);
	tabgroup * curr;
	pthread_mutex_lock(&locker_gruppi[risultatofunzione % 128]);
	curr=tabellagruppi[risultatofunzione];
	while(curr!=NULL){
		if(strcmp(curr->nomegruppo,nomegruppo)!=0)
			curr=curr->next;
		else{
			gruppo * currente=curr->gruppo;
			gruppo* quellodiprima=NULL;
			while(currente!=NULL){
				if(strcmp(currente->utente,nomeutente)==0){
					free(currente->utente);
					currente->utente=NULL;
					free(currente);
					currente=NULL;
					if(quellodiprima==NULL)
						curr->gruppo=curr->gruppo->next;
					else
						quellodiprima->next=quellodiprima->next->next;
					pthread_mutex_unlock(&locker_gruppi[risultatofunzione % 128]);
					return 1;
				}
				else{
					quellodiprima=currente;
					currente=currente->next;
				}
			}
			pthread_mutex_unlock(&locker_gruppi[risultatofunzione % 128]);
			return -1;
		}
	}
	pthread_mutex_unlock(&locker_gruppi[risultatofunzione % 128]);
	return -1;
}

listaconn * aggiungi_elemento_listaconnessi(listaconn * lista,char * string,int fd){
	listaconn * elemento=malloc(sizeof(listaconn));
	strcpy(elemento->utente,string);
	elemento->next=NULL;
	elemento->connfd=fd;
	if(lista==NULL){
		numutenticonnessi=1;
		return elemento;
	}
	listaconn *current=lista;
	while(current->next!=NULL)
		current=current->next;
	current->next=elemento;
	numutenticonnessi++;
	return lista;
}

/**
 * Funzione utilizzata per permettere ai diversi thread di prendere
 * un operazione dalla coda operazioni
 *
 */

coda * prendi_operazione(){
	while(codino==NULL){
		pthread_cond_wait(&codavuota,&mutecscodaoperazioni);
		if(siguscita_flag==0){
			return NULL;
		}
	}
	coda * elemento=codino;
	coda * ritorn=malloc(sizeof(coda));
	ritorn->messaggio=elemento->messaggio;
	ritorn->connfd=elemento->connfd;
	ritorn->next=NULL;
	codino=codino->next;
	free(elemento);
	elemento=NULL;
	return ritorn;
}

/**
 * L'utente con connfd associato si deve essere sconnesso, lo tolgo dalla lista utenti connessi
 *
 */

int eliminadallacoda(int connfd){
	listaconn * current=listaconnessi;
	if(listaconnessi!=NULL){
		if(current->connfd==connfd){
			listaconnessi=listaconnessi->next;
			free(current);
			current=NULL;
			numutenticonnessi--;
			return 0;
		}
		listaconn * primadicorr=current;
		current=current->next;
		while(current!=NULL){
			if(current->connfd==connfd){
				primadicorr->next=current->next;
				free(current);
				current=NULL;
				numutenticonnessi--;
				return 0;
			}
			else{
				primadicorr=current;
				current=current->next;
			}
		}
	}
	return 1;
}

/**
 * Funzione utilizzata per creare la stringa contenente tutti gli utenti connessi
 * al momento.
 *
 */

char * concatenalista(){
	int i;
	int k = 0;
	char * risultato=calloc((numutenticonnessi*(MAX_NAME_LENGTH+1)+1),sizeof(char));
	listaconn * current=listaconnessi;
	while(current!=NULL){
		strcat(risultato,current->utente);
		k=k+strlen(current->utente);
		for(i=0;i<MAX_NAME_LENGTH-strlen(current->utente)+1;i++){//aggiungo spazi
			risultato[k]=' ';
			k++;
		}
		current=current->next;
	}
	for(i=32;i<MAX_NAME_LENGTH*numutenticonnessi;i=i+33)
		risultato[i]='\0';
	return risultato;
}

/**
 *@brief Funzione utilizzata dai thread per lavorare.
 *
 * Ogni thread, lavorando in concorrenza, prende un elemento dalla coda operazioni
 * e esegue un operazione richiesta da un dato utente.
 *
 *
 *
 */

static void * lavora(void *args){
	int i=*((int *)args); 
	message_t mess;
	message_data_t * msg=malloc(sizeof(message_data_t));
	coda ** arraydicode=malloc(10*sizeof(coda *));
	int num=0;
	int max=10;
	while(siguscita_flag!=0){
		pthread_mutex_lock(&mutecscodaoperazioni);
		coda * su_cui_lavorare=prendi_operazione();
		pthread_mutex_unlock(&mutecscodaoperazioni);
		if(su_cui_lavorare==NULL){
			goto MAMMA;
		}
		printf("ho rilasciato la lock e sono il thread %d e ho l'fd %d\n",i,su_cui_lavorare->connfd);
		printf("ecco cosa ho preso come operazione:%d fatta da %s sono il thread %d\n",su_cui_lavorare->messaggio.hdr.op,su_cui_lavorare->messaggio.hdr.sender,i );
		switch(su_cui_lavorare->messaggio.hdr.op){
			case REGISTER_OP:{
				printf("provo a registrare\n");
				mess_t * firstmess;//utilizzata per inizializzare la parte dati della tabella hash
				firstmess=malloc(sizeof(mess_t));
				firstmess->tmess=42;//firstmess utilizzata per inizializzare la parte dati della tabella hash, la struttura si trova in icl_hash.h, 42 utilizzata solo per capire che è il primo elemento non inizializzato
				firstmess->mess=NULL;
				if((icl_hash_insert(hash, su_cui_lavorare->messaggio.hdr.sender, (void*)firstmess))==NULL){
					printf("errore inserimento %s\n",su_cui_lavorare->messaggio.hdr.sender );
					mess.hdr.op=OP_NICK_ALREADY;
                    pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
					sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                    pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
					pthread_mutex_lock(&mutecstats);
					chattyStats.nerrors++;
					pthread_mutex_unlock(&mutecstats);
				}
				else{
					pthread_mutex_lock(&mutecstats);
					chattyStats.nusers++;
					pthread_mutex_unlock(&mutecstats);
					pthread_mutex_lock(&mutecslistaconn);
					if(numutenticonnessi<MaxConnections){
						setHeader(&mess.hdr,OP_OK,"");
                        pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
						sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                        pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
						listaconnessi=aggiungi_elemento_listaconnessi(listaconnessi,su_cui_lavorare->messaggio.hdr.sender,su_cui_lavorare->connfd);
						setData(msg,"",concatenalista(),numutenticonnessi*MAX_NAME_LENGTH+numutenticonnessi);
                        pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
						sendData(su_cui_lavorare->connfd,msg);
                        pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
						free(msg->buf);
					}
					else{
						mess.hdr.op=OP_FAIL;
                        pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
						sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                        pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
						free(su_cui_lavorare);
						su_cui_lavorare=NULL;
						pthread_mutex_lock(&mutecstats);
						chattyStats.nerrors++;
						pthread_mutex_unlock(&mutecstats);
					}
					pthread_mutex_unlock(&mutecslistaconn);
				}
			}break;
			case CONNECT_OP:{
				printf("provo a connettere\n");
				if(icl_hash_find(hash,&su_cui_lavorare->messaggio.hdr.sender)!=NULL){
					pthread_mutex_lock(&mutecslistaconn);
					if(numutenticonnessi<MaxConnections){
						setHeader(&mess.hdr,OP_OK,su_cui_lavorare->messaggio.hdr.sender);
                        pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
						sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                        pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
						listaconnessi=aggiungi_elemento_listaconnessi(listaconnessi,su_cui_lavorare->messaggio.hdr.sender,su_cui_lavorare->connfd);
						memset((char*)&(msg->hdr), 0, sizeof(message_data_hdr_t));
						msg->buf=concatenalista();
						msg->hdr.len=numutenticonnessi*MAX_NAME_LENGTH+numutenticonnessi;
						strcpy(msg->hdr.receiver,"");
                        pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
						sendData(su_cui_lavorare->connfd,msg);
                        pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
						free(msg->buf);
					}
					else{
						mess.hdr.op=OP_FAIL;
                        pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
						sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                        pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
						pthread_mutex_lock(&mutecstats);
						chattyStats.nerrors++;
						pthread_mutex_unlock(&mutecstats);
					}
					pthread_mutex_unlock(&mutecslistaconn);
				}
				else{
					printf("errore connessione %s\n",su_cui_lavorare->messaggio.hdr.sender );
					mess.hdr.op=OP_NICK_UNKNOWN;
                    pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
					sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                    pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
					pthread_mutex_lock(&mutecstats);
					chattyStats.nerrors++;
					pthread_mutex_unlock(&mutecstats);
				}
			}break;
			case USRLIST_OP:{
				printf("stampo la lista\n");
				mess.hdr.op=OP_OK;
                pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
				sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
				pthread_mutex_lock(&mutecslistaconn);
				msg->buf=concatenalista();
				msg->hdr.len=numutenticonnessi*MAX_NAME_LENGTH+numutenticonnessi;
                pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
				sendData(su_cui_lavorare->connfd,msg);
                pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
				free(msg->buf);
				pthread_mutex_unlock(&mutecslistaconn);
			}break;
			case UNREGISTER_OP:{
				mess.hdr.op=OP_OK;
                pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
				sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
				pthread_mutex_lock(&mutecslistaconn);
				icl_hash_delete(hash, &su_cui_lavorare->messaggio.hdr.sender, NULL,datafreer);//elimino sempre chi ha richiesto la funizone
				int controllo=eliminadallacoda(su_cui_lavorare->connfd);
				pthread_mutex_unlock(&mutecslistaconn);
				pthread_mutex_lock(&mutecstats);
				chattyStats.nusers--;
				pthread_mutex_unlock(&mutecstats);
			}break;
			case POSTTXT_OP:{
				printf("provo ad inviare il messaggio %s\n",su_cui_lavorare->messaggio.data.buf);
				if(su_cui_lavorare->messaggio.data.hdr.len<MaxMsgSize){
					int posizione=funzionecasuale(su_cui_lavorare->messaggio.data.hdr.receiver);
					int trovato=0;
					pthread_mutex_lock(&locker_gruppi[posizione % 128]);
					tabgroup * curr=tabellagruppi[posizione];
					if(curr!=NULL){
						while(curr!=NULL){
							if(strcmp(curr->nomegruppo,su_cui_lavorare->messaggio.data.hdr.receiver)==0){
								trovato=1;
								break;
							}
							else
								curr=curr->next;
						}
					}
                    if(trovato==0){
    					pthread_mutex_unlock(&locker_gruppi[posizione % 128]);
    					if((icl_hash_mess_insert(hash,&su_cui_lavorare->messaggio.data.hdr.receiver,su_cui_lavorare->messaggio.data.buf,su_cui_lavorare->messaggio.data.hdr.len,su_cui_lavorare->messaggio.hdr.sender,MaxHistMsgs,TXT_MESSAGE))>0){
    						setHeader(&mess.hdr,OP_OK,"");
                            pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
    						sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                            pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
    						pthread_mutex_lock(&mutecslistaconn);
    						printf("ecco chi cerchi %s\n",su_cui_lavorare->messaggio.data.hdr.receiver );
    						int trovaconnfd=isconnesso(su_cui_lavorare->messaggio.data.hdr.receiver);
    						if(trovaconnfd>0){
    							setHeader(&mess.hdr,TXT_MESSAGE,su_cui_lavorare->messaggio.hdr.sender);
    							setData(&mess.data,"",su_cui_lavorare->messaggio.data.buf,su_cui_lavorare->messaggio.data.hdr.len);
                                pthread_mutex_lock(&locker_fd[trovaconnfd]);
    							sendRequest(trovaconnfd,&mess);
                                pthread_mutex_unlock(&locker_fd[trovaconnfd]);
    							pthread_mutex_lock(&mutecstats);
    							chattyStats.ndelivered++;
    							pthread_mutex_unlock(&mutecstats);
    						}
    						else{
    							pthread_mutex_lock(&mutecstats);
    							chattyStats.nnotdelivered++;
    							pthread_mutex_unlock(&mutecstats);
    						}
    						pthread_mutex_unlock(&mutecslistaconn);
    					}
    					else{
    							mess.hdr.op=OP_NICK_UNKNOWN;
                                pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
    							sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                                pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
    							printf("l'utente %s non esiste, l'utente con fd %d fallisce\n",su_cui_lavorare->messaggio.data.hdr.receiver,su_cui_lavorare->connfd);
    							pthread_mutex_lock(&mutecstats);
    							chattyStats.nerrors++;
    							pthread_mutex_unlock(&mutecstats);
    					}
    				}
                    else{//sto inviando a un gruppo
                        trovato=0;
                        gruppo * group=curr->gruppo;
                        gruppo *currente=curr->gruppo;
                        while(currente!=NULL){
                            if(strcmp(currente->utente,su_cui_lavorare->messaggio.hdr.sender)!=0)
                                currente=currente->next;
                            else{
                                trovato=1;
                                break;
                            }
                        }
                        if(trovato==0){
                            setHeader(&mess.hdr,OP_NICK_UNKNOWN,"");
                            pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
                            sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                            pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
                            pthread_mutex_unlock(&locker_gruppi[posizione % 128]);
                            pthread_mutex_lock(&mutecstats);
                            chattyStats.nerrors++;
                            pthread_mutex_unlock(&mutecstats);
                        }
                        else{//il nick e' nel gruppo, inviare a tutti
                            setHeader(&mess.hdr,OP_OK,"");
                            pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
                            sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                            pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
                            currente=group;
                            gruppo * cancellami=NULL;;
                            int cancellare=0;
                            while(currente!=NULL){
                                  printf("provo ad inviare all'utente %s del gruppo %s\n",currente->utente,su_cui_lavorare->messaggio.data.hdr.receiver );
                             //   if(strcmp(currente->utente,su_cui_lavorare->messaggio.hdr.sender)!=0){
                                    if((icl_hash_mess_insert(hash,currente->utente,su_cui_lavorare->messaggio.data.buf,su_cui_lavorare->messaggio.data.hdr.len,su_cui_lavorare->messaggio.hdr.sender,MaxHistMsgs,TXT_MESSAGE))>0){
                                        pthread_mutex_lock(&mutecslistaconn);
                                        int trovaconnfd=isconnesso(currente->utente);
                                        if(trovaconnfd>0){
                                            setHeader(&mess.hdr,TXT_MESSAGE,su_cui_lavorare->messaggio.hdr.sender);
                                            setData(&mess.data,"",su_cui_lavorare->messaggio.data.buf,su_cui_lavorare->messaggio.data.hdr.len);
                                            pthread_mutex_lock(&locker_fd[trovaconnfd]);
                                            sendRequest(trovaconnfd,&mess);
                                            pthread_mutex_unlock(&locker_fd[trovaconnfd]);
                                            pthread_mutex_lock(&mutecstats);
                                            chattyStats.ndelivered++;
                                            pthread_mutex_unlock(&mutecstats);
                                        }
                                        else{
                                            pthread_mutex_lock(&mutecstats);
                                            chattyStats.nnotdelivered++;
                                            pthread_mutex_unlock(&mutecstats);
                                        }
                                        pthread_mutex_unlock(&mutecslistaconn);
                                    }
                                    else//ho deregistrato quell'utente
                                        cancellare=1;
                           //     }
                                    if(cancellare==0){
                                        cancellami=currente;
                                        currente=currente->next;
                                    }
                                    else{
                                        cancellare=0;
                                        if(cancellami!=NULL){
                                            currente=currente->next;
                                            free(cancellami->next->utente);
                                            free(cancellami->next);
                                            cancellami->next=currente;
                                        }
                                        else{
                                            cancellami=currente;
                                            currente=currente->next;
                                            curr->gruppo=currente;
                                            free(cancellami->utente);
                                            free(cancellami);
                                            cancellami=NULL;
                                        }
                                    }
                            }
                            pthread_mutex_unlock(&locker_gruppi[posizione % 128]);
                        }
                    }
                }
				else{
					mess.hdr.op=OP_MSG_TOOLONG;
                    pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
					sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                    pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
					printf("mess troppo lungo\n");
					pthread_mutex_lock(&mutecstats);
					chattyStats.nerrors++;
					pthread_mutex_unlock(&mutecstats);
				}
			}break;
			case POSTTXTALL_OP:{
				if(su_cui_lavorare->messaggio.data.hdr.len<MaxMsgSize){
					printf("invio %s a tutti\n",su_cui_lavorare->messaggio.data.buf );
					setHeader(&mess.hdr,OP_OK,"");
                    pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
					sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                    pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
					inviamessatutti(hash,su_cui_lavorare->messaggio.data.buf,su_cui_lavorare->messaggio.data.hdr.len,su_cui_lavorare->messaggio.hdr.sender,MaxHistMsgs);
					pthread_mutex_lock(&mutecslistaconn);
					//scorrere lista e inviare a tutti
					listaconn *curr=listaconnessi;
					while(curr!=NULL){
						if(strcmp(curr->utente,su_cui_lavorare->messaggio.hdr.sender)!=0){
						 		setHeader(&mess.hdr,TXT_MESSAGE,su_cui_lavorare->messaggio.hdr.sender);
						 		setData(&mess.data,curr->utente,su_cui_lavorare->messaggio.data.buf,su_cui_lavorare->messaggio.data.hdr.len);
                                pthread_mutex_lock(&locker_fd[curr->connfd]);
						 		sendRequest(curr->connfd,&mess);
                                pthread_mutex_unlock(&locker_fd[curr->connfd]);
						 		pthread_mutex_lock(&mutecstats);
								chattyStats.ndelivered++;
								pthread_mutex_unlock(&mutecstats);
						}
						 curr=curr->next;
					}
					pthread_mutex_lock(&mutecstats);
					chattyStats.nnotdelivered=chattyStats.nusers - numutenticonnessi -1;
					pthread_mutex_unlock(&mutecstats);
					pthread_mutex_unlock(&mutecslistaconn);
				}
				else{
					mess.hdr.op=OP_MSG_TOOLONG;
                    pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
					sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                    pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
					printf("mess troppo lungo\n");
					pthread_mutex_lock(&mutecstats);
					chattyStats.nerrors++;
					pthread_mutex_unlock(&mutecstats);
				}
			}break;
			case GETPREVMSGS_OP:{
					setHeader(&mess.hdr,OP_OK,"");
                    pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
					sendHeader(su_cui_lavorare->connfd,&mess.hdr);
					int n=inviatuttiimess(hash,&su_cui_lavorare->messaggio.hdr.sender,su_cui_lavorare->connfd);
                    pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
					pthread_mutex_lock(&mutecstats);
					chattyStats.ndelivered=chattyStats.ndelivered+n;
					pthread_mutex_unlock(&mutecstats);
			}break;
			case POSTFILE_OP:{
				printf("provo a postare un file\n");
                pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
				readData(su_cui_lavorare->connfd,msg);
                pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
				if(msg->hdr.len>MaxFileSize*1024){
					mess.hdr.op=OP_MSG_TOOLONG;
                    pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
					sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                    pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
					pthread_mutex_lock(&mutecstats);
					chattyStats.nerrors++;
					pthread_mutex_unlock(&mutecstats);
					free(msg->buf);	
					msg->buf=NULL;
				}
				else{
					if(su_cui_lavorare->messaggio.data.buf[0]!='.' && su_cui_lavorare->messaggio.data.buf[0]!='/' ){//se mi viene dato semplicemente il nome del file allora assumo che si trovi nella stessa cartella dove eseguo chatty
						su_cui_lavorare->messaggio.data.buf=realloc(su_cui_lavorare->messaggio.data.buf,(su_cui_lavorare->messaggio.data.hdr.len+2)*sizeof(char));
						su_cui_lavorare->messaggio.data.buf[su_cui_lavorare->messaggio.data.hdr.len-1]='\0';
						for(int y=su_cui_lavorare->messaggio.data.hdr.len-1; y>=0; y-- ){
							su_cui_lavorare->messaggio.data.buf[y+2]=su_cui_lavorare->messaggio.data.buf[y];
						}
						su_cui_lavorare->messaggio.data.buf[0]='.';
						su_cui_lavorare->messaggio.data.buf[1]='/';
					}
					printf("path file %s\n",su_cui_lavorare->messaggio.data.buf );//in msg->buf c'e'il file
					char *nomefile=strrchr(su_cui_lavorare->messaggio.data.buf,'/');
					char *newpath=malloc((strlen(nomefile)+strlen(DirName)+1)*sizeof(char));
					strcpy(newpath,DirName);
					newpath=strcat(newpath,nomefile);
					printf("path del file scaricato %s\n",newpath );
					int len=strlen(newpath)+1;
					FILE *fd;
					fd=fopen(newpath,"wb+");
					if(fd==NULL){
						perror("failopen");
						return -1;
					}
					msg->buf=realloc(msg->buf,sizeof(char)*(msg->hdr.len+1));
					msg->buf[msg->hdr.len]='\0';
					fwrite(msg->buf,1,msg->hdr.len,fd);
					printf("%d la lunghezza del file\n",(int)strlen(msg->buf) );
					fclose(fd);
                    int posizione=funzionecasuale(su_cui_lavorare->messaggio.data.hdr.receiver);
                    int trovato=0;
                    pthread_mutex_lock(&locker_gruppi[posizione % 128]);
                    tabgroup * curr=tabellagruppi[posizione];
                    if(curr!=NULL){
                        while(curr!=NULL){
                            if(strcmp(curr->nomegruppo,su_cui_lavorare->messaggio.data.hdr.receiver)==0){
                                trovato=1;
                                break;
                            }
                            else
                                curr=curr->next;
                        }
                    }
                    if(trovato==0){
                        pthread_mutex_unlock(&locker_gruppi[posizione % 128]);
    					if((icl_hash_mess_insert(hash,&su_cui_lavorare->messaggio.data.hdr.receiver,newpath,len,su_cui_lavorare->messaggio.hdr.sender,MaxHistMsgs,FILE_MESSAGE))>0){
    						setHeader(&mess.hdr,OP_OK,"");
                            pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
    						sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                            pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
                            pthread_mutex_lock(&mutecslistaconn);
    						int trovaconnfd=isconnesso(su_cui_lavorare->messaggio.data.hdr.receiver);
    						if(trovaconnfd>0){
    							setHeader(&mess.hdr,FILE_MESSAGE,su_cui_lavorare->messaggio.hdr.sender);
    							setData(&mess.data,"",newpath,len);
                                pthread_mutex_lock(&locker_fd[trovaconnfd]);
    							sendRequest(trovaconnfd,&mess);
                                pthread_mutex_unlock(&locker_fd[trovaconnfd]);
    							pthread_mutex_lock(&mutecstats);
    							chattyStats.nfiledelivered++;
    							pthread_mutex_unlock(&mutecstats);
    						}
    						else{
    							pthread_mutex_lock(&mutecstats);
    							chattyStats.nfilenotdelivered++;
    							pthread_mutex_unlock(&mutecstats);
    						}
                            pthread_mutex_unlock(&mutecslistaconn);
    						free(newpath);
    						newpath=NULL;
    						free(msg->buf);
    						msg->buf=NULL;
    					}
    					else{
    						printf("utente non trovato\n");
    						mess.hdr.op=OP_NICK_UNKNOWN;
                            pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
    						sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                            pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
    						pthread_mutex_lock(&mutecstats);
    						chattyStats.nerrors++;
    						pthread_mutex_unlock(&mutecstats);
    						free(newpath);
    						free(msg->buf);
    						newpath=NULL;
    						msg->buf=NULL;
    					}
    				}
                    else{// sto inviando a un gruppo
                        trovato=0;
                        gruppo * group=curr->gruppo;
                        gruppo *currente=curr->gruppo;
                        while(currente!=NULL){
                            if(strcmp(currente->utente,su_cui_lavorare->messaggio.hdr.sender)!=0)
                                currente=currente->next;
                            else{
                                trovato=1;
                                break;
                            }
                        }
                        if(trovato==0){
                            setHeader(&mess.hdr,OP_NICK_UNKNOWN,"");
                            pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
                            sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                            pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
                            pthread_mutex_unlock(&locker_gruppi[posizione % 128]);
                            pthread_mutex_lock(&mutecstats);
                            chattyStats.nerrors++;
                            pthread_mutex_unlock(&mutecstats);
                        }
                        else{//il nick e' nel gruppo inviare a tutti
                            setHeader(&mess.hdr,OP_OK,"");
                            pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
                            sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                            pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
                            gruppo * cancellami=NULL;;
                            int cancellare=0;
                            currente=group;
                            while(currente!=NULL){
                         //       if(strcmp(currente->utente,su_cui_lavorare->messaggio.hdr.sender)!=0){
                                    printf("provo ad inviare all'utente %s del gruppo %s\n",currente->utente,su_cui_lavorare->messaggio.data.hdr.receiver );
                                    if((icl_hash_mess_insert(hash,currente->utente,newpath,len,su_cui_lavorare->messaggio.hdr.sender,MaxHistMsgs,FILE_MESSAGE))>0){
                                        pthread_mutex_lock(&mutecslistaconn);
                                        int trovaconnfd=isconnesso(su_cui_lavorare->messaggio.data.hdr.receiver);
                                        if(trovaconnfd>0){
                                            setHeader(&mess.hdr,FILE_MESSAGE,su_cui_lavorare->messaggio.hdr.sender);
                                            setData(&mess.data,"",newpath,len);
                                            pthread_mutex_lock(&locker_fd[trovaconnfd]);
                                            sendRequest(trovaconnfd,&mess);
                                            pthread_mutex_unlock(&locker_fd[trovaconnfd]);
                                            pthread_mutex_lock(&mutecstats);
                                            chattyStats.nfiledelivered++;
                                            pthread_mutex_unlock(&mutecstats);
                                        }
                                        else{
                                               pthread_mutex_lock(&mutecstats);
                                               chattyStats.nfilenotdelivered++;
                                               pthread_mutex_unlock(&mutecstats);
                                        }
                                        pthread_mutex_unlock(&mutecslistaconn);
                                    }
                                    else{//il tizio si sara' deregistrato
                                        cancellare=1;
                                    }
                       //         }
                                if(cancellare==0){
                                    cancellami=currente;
                                    currente=currente->next;
                                }
                                else{
                                    cancellare=0;
                                    if(cancellami!=NULL){
                                        currente=currente->next;
                                        free(cancellami->next->utente);
                                        free(cancellami->next);
                                        cancellami->next=currente;
                                    }
                                    else{
                                        cancellami=currente;
                                        currente=currente->next;
                                        curr->gruppo=currente;
                                        free(cancellami->utente);
                                        free(cancellami);
                                        cancellami=NULL;
                                    }
                                }
                            }
                            pthread_mutex_unlock(&locker_gruppi[posizione % 128]);
                            free(newpath);
                            newpath=NULL;
                            free(msg->buf);
                            msg->buf=NULL;
                        }
                    }
    				free(su_cui_lavorare->messaggio.data.buf);
    				su_cui_lavorare->messaggio.data.buf=NULL;
                }
			}break;
			case GETFILE_OP:{
				printf("provo a prendere il file richiesto %s\n",su_cui_lavorare->messaggio.data.buf);
				FILE *ffd;
				ffd=fopen(su_cui_lavorare->messaggio.data.buf,"r");
				if(ffd==NULL){
					printf("il file non esiste\n");
					mess.hdr.op=OP_NO_SUCH_FILE;
                    pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
					sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                    pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
					pthread_mutex_lock(&mutecstats);
					chattyStats.nerrors++;
					pthread_mutex_unlock(&mutecstats);
				}
				else{
					msg->buf=malloc(sizeof(char)*su_cui_lavorare->messaggio.data.hdr.len);
					if(fgets(msg->buf,su_cui_lavorare->messaggio.data.hdr.len,ffd)==NULL){
						printf("errore lettura file\n");
						mess.hdr.op=OP_FAIL;
                        pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
						sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                        pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
						pthread_mutex_lock(&mutecstats);
						chattyStats.nerrors++;
						pthread_mutex_unlock(&mutecstats);
						fclose(ffd);
						free(msg->buf);
						msg->buf=NULL;
					}
					else{
						setHeader(&mess.hdr,OP_OK,"");
						setData(&mess.data,"",msg->buf,ftell(ffd));
                        pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
                        sendHeader(su_cui_lavorare->connfd,&mess.hdr);
						sendData(su_cui_lavorare->connfd,&mess.data);
                        pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
						free(msg->buf);
						msg->buf=NULL;
						fclose(ffd);
						pthread_mutex_lock(&mutecstats);
						chattyStats.nfiledelivered++;
						pthread_mutex_unlock(&mutecstats);
					}
				}
			}break;
			case CREATEGROUP_OP:{
				if(strlen(su_cui_lavorare->messaggio.data.hdr.receiver)==0){
					setHeader(&mess.hdr,OP_FAIL,"");
                    pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
					sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                    pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
				}
				else{
					int ris=inesertgruppo(su_cui_lavorare->messaggio.data.hdr.receiver,su_cui_lavorare->messaggio.hdr.sender);
					if(ris>=0){
						setHeader(&mess.hdr,OP_OK,"");
                        pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
						sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                        pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
					}
					else{
						setHeader(&mess.hdr,OP_NICK_ALREADY,"");
                        pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
						sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                        pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
					}
				}
			}break;
			case ADDGROUP_OP:{
				if(searchandinsert(su_cui_lavorare->messaggio.hdr.sender,su_cui_lavorare->messaggio.data.hdr.receiver)>0){
					setHeader(&mess.hdr,OP_OK,"");
                    pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
					sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                    pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
				}
				else{
					setHeader(&mess.hdr,OP_FAIL,"");
                    pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
					sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                    pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
				}
			}break;
			case DELGROUP_OP:{
				if(searchanddelete(su_cui_lavorare->messaggio.hdr.sender,su_cui_lavorare->messaggio.data.hdr.receiver)>0){
					setHeader(&mess.hdr,OP_OK,"");
                    pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
					sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                    pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
				}
				else{
					setHeader(&mess.hdr,OP_FAIL,"");
                    pthread_mutex_lock(&locker_fd[su_cui_lavorare->connfd]);
					sendHeader(su_cui_lavorare->connfd,&mess.hdr);
                    pthread_mutex_unlock(&locker_fd[su_cui_lavorare->connfd]);
				}
			}break;
			default:{
				printf("ciao\n");
			}
		}
		pthread_mutex_lock(&mutecsfmax);
		FD_SET(su_cui_lavorare->connfd,&set);
		if(su_cui_lavorare->connfd > fdmax) fdmax = su_cui_lavorare->connfd;
		pthread_mutex_unlock(&mutecsfmax);
		free(su_cui_lavorare->messaggio.data.buf);
		su_cui_lavorare->messaggio.data.buf=NULL;
		arraydicode[num]=su_cui_lavorare;
		num++;
		if(num>=max){
			max=max+10;
			arraydicode=realloc(arraydicode,sizeof(coda*)*max);
		}
	}
	MAMMA:
	free(msg); 
	for(int p=0; p<num;p++)
		free(arraydicode[p]);
	free(arraydicode);
	return (void *) 0;
}
/**
 *
 *@brief Funzione per inserire un nuovo elemento nella coda operazioni
 *
 *@param[in] lista -- la lista degli utenti connessi
 *@param[in] msg -- il messaggio inviaro dall'utente
 *@param[in] indice -- il connfd dell'utente
 *
 * 
 *
 */

coda * aggiungi_elemento(coda* lista, message_t msg, int indice){
	printf("entro nella funzione aggiungi_elemento\n");
	coda *elemento=malloc(sizeof(coda));
	elemento->messaggio=msg;
	elemento->connfd=indice;
	elemento->next=NULL;
	if(lista==NULL){
		printf("esco\n");
		return elemento; 
	}
	coda *current=lista;
	while(current->next!=NULL){
		current=current->next;
	}
	current->next=elemento;
	printf("esco\n");
	return lista;
}

static void usage(const char *progname) {
    fprintf(stderr, "Il server va lanciato con il seguente comando:\n");
    fprintf(stderr, "  %s -f conffile\n", progname);
}

/**
 *
 * Funzione di hashing della tabella hash degli utenti registrati
 *
 *
 */

static unsigned int hash_pjw(void* key)
{
    char *datum = (char *)key;
    unsigned int hash_value, i;

    if(!datum) return 0;

    for (hash_value = 0; *datum; ++datum) {
        hash_value = (hash_value << ONE_EIGHTH) + *datum;
        if ((i = hash_value & HIGH_BITS) != 0)
            hash_value = (hash_value ^ (i >> THREE_QUARTERS)) & ~HIGH_BITS;
    }
    return (hash_value);
}

static int string_compare(void* a, void* b)
{
    return (strcmp( (char*)a, (char*)b ) == 0);
}

int main(int argc, char *argv[]) {
  FILE *configurazione;
  if(argv[2]==NULL)
  	usage("chatty");
  configurazione=fopen(argv[2],"r");
  UnixPath=parsastringa("UnixPath",configurazione);
  DirName=parsastringa("DirName",configurazione);
  StatFileName=parsastringa("StatFileName",configurazione);
  MaxMsgSize=parsanumero("MaxMsgSize",configurazione);
  MaxConnections=parsanumero("MaxConnections",configurazione);
  MaxHistMsgs=parsanumero("MaxHistMsgs",configurazione);
  MaxFileSize=parsanumero("MaxFileSize",configurazione);
  ThreadsInPool=parsanumero("ThreadsInPool",configurazione);
  fclose(configurazione);// qui finisce il parsing continua a fare codice da qua
  char *indirizzo=strcat(UnixPath,"/");
  indirizzo=strcat(indirizzo,SockName);
  unlink(indirizzo);
  stat=fopen(StatFileName,"w");
  int i;
  message_t msg;//messaggio con cui comunico
  struct sigaction s;
  memset( &s, 0, sizeof(s) );
  s.sa_handler=gestore;
  siguscita_flag=1;
  sigaction(SIGTERM,&s,NULL);
  sigaction(SIGQUIT,&s,NULL);
  sigaction(SIGUSR1,&s,NULL);
  struct sigaction esse;
  memset(&esse, 0, sizeof(esse));
  esse.sa_handler=SIG_IGN;
  sigaction(SIGPIPE,&esse,NULL);
  codino=NULL;
  listaconnessi=NULL;
  hash = icl_hash_create(512, hash_pjw, string_compare); //creo tabella hash
  if(hash!=NULL)
  	printf("creazione tabella hash riuscita\n"); 
  tabellagruppi=malloc(256*sizeof(tabgroup *)); //creo la tabella con i gruppi
  for(i=0; i<256; i++){
  	tabellagruppi[i]=NULL;
  }
  locker_gruppi=malloc((256/2)*sizeof(pthread_mutex_t));
  for(int k=0; k<(256/2); k++)
  	pthread_mutex_init(&locker_gruppi[k], NULL);//mutex per tabella gruppi
  locker_fd=malloc((6+MaxConnections)*sizeof(pthread_mutex_t));
  for(int k=0; k<(6+MaxConnections); k++)
    pthread_mutex_init(&locker_fd[k], NULL); //mutex read e write
  pthread_t lavoratori[ThreadsInPool];
  for(i=0; i<ThreadsInPool; i++)
  	if(pthread_create(&lavoratori[i],NULL,&lavora,(void *)&i)!=0){
  		printf("errore creazione trhead\n");
  		return -1;
  	}
  int listenfd=socket(AF_UNIX,SOCK_STREAM,0);
  if(listenfd<0){
    perror("socket");
    return -1;
  }
  struct sockaddr_un serv_addr;
  memset(&serv_addr, '0',sizeof(serv_addr));
  serv_addr.sun_family= AF_UNIX;
  strncpy(serv_addr.sun_path,indirizzo,strlen(indirizzo)+1);
  printf("l'indirizzo è %s\n",indirizzo);
  int notused;
  notused=bind(listenfd,(struct sockaddr *)&serv_addr,sizeof(serv_addr));
  if(notused<0){
    perror("bind");
    return -1;
  }
  notused=listen(listenfd,SOMAXCONN);
    if(notused<0){
      perror("listen");
      return -1;
    }
   struct timeval * timer=malloc(sizeof(struct timeval)); //faccio un timer per disconnettersi nel caso smettano di esserci richieste e debba disconnetermi
   timer->tv_sec=0;
   timer->tv_usec=50000;
    fd_set tmpset;
    // azzero sia il master set che il set temporaneo usato per la select
    FD_ZERO(&set);
    FD_ZERO(&tmpset);
    // aggiungo il listener fd al master set
    FD_SET(listenfd, &set);
    fdmax = listenfd;
    while(siguscita_flag!=0) {      
		timer->tv_sec=0;
		timer->tv_usec=15000;
		// copio il set nella variabile temporanea per la select
		pthread_mutex_lock(&mutecsfmax);
		tmpset = set;
		pthread_mutex_unlock(&mutecsfmax); 
		int notused;
		notused=select(fdmax+1, &tmpset, NULL, NULL, timer); //se va in loop devo ricordarmi che è il timer
		if(notused<0){
	  		perror("select");
	  		notused=0;
		}
	// cerchiamo di capire da quale fd abbiamo ricevuto una richiesta
		if(notused!=0){
			for(int i=0; i <= fdmax; i++) {
		    	if (FD_ISSET(i, &tmpset)) {
					long connfd;
					if (i == listenfd) { // e' una nuova richiesta di connessione
			    		connfd=accept(listenfd, (struct sockaddr*)NULL ,NULL);
			    		if(connfd<0){
			      			perror("accept");
			     		 	return -1;
			    		}
			    		pthread_mutex_lock(&mutecsfmax);
			   	 		FD_SET(connfd, &set);
			    		if(connfd > fdmax) fdmax = connfd;
			    		pthread_mutex_unlock(&mutecsfmax);
					}
					else {
			    		connfd = i;  // e' una nuova richiesta da un client già connesso
					}
			// eseguo il comando e se c'e' un errore lo tolgo dal master set
    				msg.data.buf=NULL;
                    pthread_mutex_lock(&locker_fd[connfd]);
    				int prova=readMsg(connfd,&msg);
                    pthread_mutex_unlock(&locker_fd[connfd]);
    				if(prova<=0){
    					pthread_mutex_lock(&mutecsfmax);
    					close(connfd);
    					FD_CLR(connfd,&set);
    					pthread_mutex_unlock(&mutecsfmax);
    					pthread_mutex_lock(&mutecslistaconn);
    					printf("ho la mutex delle connessioni nel main\n");
    					int controllo=eliminadallacoda(connfd);
    					if(msg.data.buf!=NULL){
    						free(msg.data.buf);
    						msg.data.buf=NULL;
    					}
    					pthread_mutex_unlock(&mutecslistaconn);
    					printf("l'ho lasciata\n");
    				}
    				else{
    					printf("%d ecco l'operazione\n",msg.hdr.op);
    					printf("%s ecco chi la richiede\n",msg.hdr.sender );
    					pthread_mutex_lock(&mutecscodaoperazioni);
    					printf("ho preso mutex nel main\n");
    					pthread_mutex_lock(&mutecsfmax);
    					FD_CLR(connfd,&set);
    					pthread_mutex_unlock(&mutecsfmax);
    					codino=aggiungi_elemento(codino,msg,connfd);
    					pthread_cond_signal(&codavuota);
    					pthread_mutex_unlock(&mutecscodaoperazioni);
    					printf("ho lasciato la mutex dal main\n");
    				}
    		    	// devo reimpostare il massimo 
    		    	pthread_mutex_lock(&mutecsfmax);
    				if (connfd > fdmax){ 
    					fdmax = updatemax(set, fdmax);
    				}
    				pthread_mutex_unlock(&mutecsfmax);
			    }
		    }
	    }
    }
  close(listenfd);
  pthread_cond_broadcast(&codavuota);
  for(i=0; i<ThreadsInPool;i++){
	  if(pthread_join(lavoratori[i],NULL)!=0){
	        	perror("errore con la join");
	  }
  }
  icl_hash_destroy(hash,NULL,datafreer);
  printf("liberando la memoria\n");
  tabgroup *curr;
  gruppo *currente;
  for(i=0; i<256; i++){
    if (tabellagruppi[i]!=NULL){
        curr=tabellagruppi[i];
        while(curr!=NULL){
            free(curr->nomegruppo);
            currente=curr->gruppo;
            while(currente!=NULL){
                free(currente->utente);
                currente=currente->next;
            }
            free(curr->gruppo);
            curr=curr->next;
        }
        free(tabellagruppi[i]);
    }
  }
  free(UnixPath);
  free(DirName);
  fclose(stat);
  free(StatFileName);
  free(timer);
  free(locker_gruppi);
  free(locker_fd);
  printf("addio mondo\n");
  return 0;
}