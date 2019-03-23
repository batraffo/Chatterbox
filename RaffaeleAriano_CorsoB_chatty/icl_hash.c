/**
 * @file icl_hash.c
 *
 * Dependency free hash table implementation.
 *
 * This simple hash table implementation should be easy to drop into
 * any other peice of code, it does not depend on anything else :-)
 * 
 * @author Jakub Kurzak
 */
/* $Id: icl_hash.c 2838 2011-11-22 04:25:02Z mfaverge $ */
/* $UTK_Copyright: $ */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>


#include "icl_hash.h"

#include <limits.h>

#include <pthread.h>

#include <message.h>
#include <connections.h>

#define BITS_IN_int     ( sizeof(int) * CHAR_BIT )
#define THREE_QUARTERS  ((int) ((BITS_IN_int * 3) / 4))
#define ONE_EIGHTH      ((int) (BITS_IN_int / 8))
#define HIGH_BITS       ( ~((unsigned int)(~0) >> ONE_EIGHTH ))


static pthread_mutex_t *locker_hash;
extern int MaxHistMsgs;

/**
 * A simple string hash.
 *
 * An adaptation of Peter Weinberger's (PJW) generic hashing
 * algorithm based on Allen Holub's version. Accepts a pointer
 * to a datum to be hashed and returns an unsigned integer.
 * From: Keith Seymour's proxy library code
 *
 * @param[in] key -- the string to be hashed
 *
 * @returns the hash index
 */
static unsigned int
hash_pjw(void* key)
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


/**
 * Create a new hash table.
 *
 * @param[in] nbuckets -- number of buckets to create
 * @param[in] hash_function -- pointer to the hashing function to be used
 * @param[in] hash_key_compare -- pointer to the hash key comparison function to be used
 *
 * @returns pointer to new hash table.
 */

icl_hash_t *
icl_hash_create( int nbuckets, unsigned int (*hash_function)(void*), int (*hash_key_compare)(void*, void*) )
{
    icl_hash_t *ht;
    int i;
    locker_hash=malloc((nbuckets/4)*sizeof(pthread_mutex_t));
    for(i=0; i<(nbuckets/4); i++)
        pthread_mutex_init(&locker_hash[i], NULL);
    ht = (icl_hash_t*) malloc(sizeof(icl_hash_t));
    if(!ht) return NULL;

    ht->nentries = 0;
    ht->buckets = (icl_entry_t**)malloc(nbuckets * sizeof(icl_entry_t*));
    if(!ht->buckets) return NULL;

    ht->nbuckets = nbuckets;
    for(i=0;i<ht->nbuckets;i++)
        ht->buckets[i] = NULL;

    ht->hash_function = hash_function ? hash_function : hash_pjw;
    ht->hash_key_compare = hash_key_compare ? hash_key_compare : string_compare;

    return ht;
}

/**
 * Search for an entry in a hash table.
 *
 * @param ht -- the hash table to be searched
 * @param key -- the key of the item to search for
 *
 * @returns pointer to the data corresponding to the key.
 *   If the key was not found, returns NULL.
 */

void *
icl_hash_find(icl_hash_t *ht, void* key)
{
    icl_entry_t* curr;
    unsigned int hash_val;

    if(!ht || !key) return NULL;

    hash_val = (* ht->hash_function)(key) % ht->nbuckets;
    pthread_mutex_lock(&locker_hash[hash_val % 128]);
    for (curr=ht->buckets[hash_val]; curr != NULL; curr=curr->next){
        if ( ht->hash_key_compare(curr->key, key)){
            pthread_mutex_unlock(&locker_hash[hash_val % 128]);
            return(curr->data);
        }
    }
    pthread_mutex_unlock(&locker_hash[hash_val % 128]);
    return NULL;
}

/**
 * Insert an item into the hash table.
 *
 * @param ht -- the hash table
 * @param key -- the key of the new item
 * @param data -- pointer to the new item's data
 *
 * @returns pointer to the new item.  Returns NULL on error.
 */

icl_entry_t *
icl_hash_insert(icl_hash_t *ht, void* key, void *data)
{
    icl_entry_t *curr;
    unsigned int hash_val;

    if(!ht || !key) return NULL;

    hash_val = (* ht->hash_function)(key) % ht->nbuckets;
    pthread_mutex_lock(&locker_hash[hash_val % 128]);
    for (curr=ht->buckets[hash_val]; curr != NULL; curr=curr->next){
        if ( ht->hash_key_compare(curr->key, key)){
            pthread_mutex_unlock(&locker_hash[hash_val % 128]);
            return(NULL); /* key already exists */
        }
    }

    /* if key was not found */
    curr = (icl_entry_t*)malloc(sizeof(icl_entry_t));
    if(!curr){
        pthread_mutex_unlock(&locker_hash[hash_val % 128]);
        return NULL;
    }
    curr->numess=0;
    curr->key = key;
    curr->data = data;
    curr->next = ht->buckets[hash_val]; /* add at start */

    ht->buckets[hash_val] = curr;
    ht->nentries++;
    pthread_mutex_unlock(&locker_hash[hash_val % 128]);
    return curr;
}

/**
 * Free one hash table entry located by key (key and data are freed using functions).
 *
 * @param ht -- the hash table to be freed
 * @param key -- the key of the new item
 * @param free_key -- pointer to function that frees the key
 * @param free_data -- pointer to function that frees the data
 *
 * @returns 0 on success, -1 on failure.
 */
int icl_hash_delete(icl_hash_t *ht, void* key, void (*free_key)(void*), void (*free_data)(void*))
{
    icl_entry_t *curr, *prev;
    unsigned int hash_val;

    if(!ht || !key) return -1;
    hash_val = (* ht->hash_function)(key) % ht->nbuckets;
    pthread_mutex_lock(&locker_hash[hash_val % 128]);
    prev = NULL;
    for (curr=ht->buckets[hash_val]; curr != NULL; )  {
        if ( ht->hash_key_compare(curr->key, key)) {
            if (prev == NULL) {
                ht->buckets[hash_val] = curr->next;
            } else {
                prev->next = curr->next;
            }
            if (*free_key && curr->key) (*free_key)(curr->key);
            if (*free_data && curr->data) (*free_data)(curr->data);
            ht->nentries++;
            free(curr);
            pthread_mutex_unlock(&locker_hash[hash_val % 128]);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }
    pthread_mutex_unlock(&locker_hash[hash_val % 128]);
    return -1;
}

/**
 * Free hash table structures (key and data are freed using functions).
 *
 * @param ht -- the hash table to be freed
 * @param free_key -- pointer to function that frees the key
 * @param free_data -- pointer to function that frees the data
 *
 * @returns 0 on success, -1 on failure.
 */
int
icl_hash_destroy(icl_hash_t *ht, void (*free_key)(void*), void (*free_data)(void*))
{
    icl_entry_t *bucket, *curr, *next;
    int i;

    if(!ht) return -1;

    for (i=0; i<ht->nbuckets; i++) {
        bucket = ht->buckets[i];
        for (curr=bucket; curr!=NULL; ) {
            next=curr->next;
            if (*free_key && curr->key) (*free_key)(curr->key);
            if (*free_data && curr->data) (*free_data)(curr->data);
            free(curr);
            curr=next;
        }
    }

    if(ht->buckets) free(ht->buckets);
    if(ht) free(ht);
    free(locker_hash);

    return 0;
}

/**
 * Dump the hash table's contents to the given file pointer.
 *
 * @param stream -- the file to which the hash table should be dumped
 * @param ht -- the hash table to be dumped
 *
 * @returns 0 on success, -1 on failure.
 */

int
icl_hash_dump(FILE* stream, icl_hash_t* ht)
{
    icl_entry_t *bucket, *curr;
    int i;

    if(!ht) return -1;

    for(i=0; i<ht->nbuckets; i++) {
        bucket = ht->buckets[i];
        for(curr=bucket; curr!=NULL; ) {
            if(curr->key)
                fprintf(stream, "icl_hash_dump: %s: %p\n", (char *)curr->key, curr->data);
            curr=curr->next;
        }
    }

    return 0;
}

/**
 * Inserisco il messaggio ricevuto da un dato utente nella sua parte dati nella tabella hash
 *
 * @param ht -- la tabella hash
 * @param key -- l'utente (che sarebbe la key poi)
 * @param toinsert -- il messaggio da inserire
 * @param len -- la lunghezza del messaggio
 * @param sender -- il mittente
 * @param maxin -- il numero max di messaggi di cui ogni utente tiene traccia
 * @param op -- specifico se il messaggio e' una stringa o un file
 *
 * @returns la lunghezza del messaggio, -1 in caso di fallimento.
 */

int
 icl_hash_mess_insert(icl_hash_t *ht,void * key ,char * toinsert,int len,char * sender,int maxin, op_t op){
    mess_t * aggiungoroba=NULL;
    icl_entry_t* curre;
    unsigned int hash_val;

    if(!ht || !key) return 0;

    hash_val = (* ht->hash_function)(key) % ht->nbuckets;
    pthread_mutex_lock(&locker_hash[hash_val % 128]);
    for (curre=ht->buckets[hash_val]; curre != NULL; curre=curre->next){
        if ( strcmp(curre->key, key)==0){
            aggiungoroba=(mess_t *)curre->data;
            break;
        }
    }
    if(aggiungoroba==NULL){printf("errore inserimento messaggio nella tabella hash\n");pthread_mutex_unlock(&locker_hash[hash_val % 128]); return 0;}
    mess_t *curr;
    curr=aggiungoroba;
    if(curre->numess==0){
        aggiungoroba->tmess=op;
        aggiungoroba->lenm=len;
        aggiungoroba->mess=malloc(sizeof(char)*len);
        strcpy(aggiungoroba->mess,toinsert);
        aggiungoroba->sender=malloc(sizeof(char)*33);
        strcpy(aggiungoroba->sender,sender);
        aggiungoroba->next=NULL;
        curre->numess=1;
        pthread_mutex_unlock(&locker_hash[hash_val % 128]);
        return len;
    }
    else{
        while(curr->next!=NULL){
            curr=curr->next;
        }
        mess_t *elemento= malloc(sizeof(mess_t));
        elemento->mess=malloc(sizeof(char)*len);
        strcpy(elemento->mess,toinsert);
        elemento->lenm=len;
        elemento->tmess=op;
        elemento->sender=malloc(sizeof(char)*33);
        strcpy(elemento->sender,sender);
        elemento->next=NULL;
        curr->next=elemento;
        curre->numess++;
        if(curre->numess>maxin){
            curre->data=(mess_t *)aggiungoroba->next;
            free(aggiungoroba->mess);
            aggiungoroba->mess=NULL;
            free(aggiungoroba->sender);
            aggiungoroba->sender=NULL;
            free(aggiungoroba);
            aggiungoroba=NULL;
            curre->numess--;
        }
        pthread_mutex_unlock(&locker_hash[hash_val % 128]);
        return len;
    }
    pthread_mutex_unlock(&locker_hash[hash_val % 128]);
    return 1;
}

/**
 * Invio tutti i messaggi, di cui sto tenendo traccia attualmente, all'utente associato a key
 *
 * @param ht -- la tabella hash
 * @param key -- l'utente (che sarebbe la key poi)
 * @param connfd -- il descrittore della connessione associato all'utente
 *
 * @returns il num di messaggi inviati all'utente
 */

int inviatuttiimess(icl_hash_t *ht, void * key,int connfd){
    message_t m;
    int num_txt=0;
    mess_t * aggiungoroba=NULL;
    mess_t * curr;
    int n=-1;
    int i;
    icl_entry_t* curre;
    unsigned int hash_val;


    if(!ht || !key) return;

    hash_val = (* ht->hash_function)(key) % ht->nbuckets;
    pthread_mutex_lock(&locker_hash[hash_val % 128]);
    for (curre=ht->buckets[hash_val]; curre != NULL; curre=curre->next){
        if ( strcmp(curre->key, key)==0){
            aggiungoroba=(mess_t *)curre->data;
            n=curre->numess;
            break;
        }
    }
    size_t saiz=(size_t) n;
    setData(&m.data,"",(char *) &saiz,sizeof(size_t));
    sendData(connfd,&m.data);
    if(aggiungoroba==NULL){printf("errore nella parte dati tabella hash\n"); free(m.data.buf) ;pthread_mutex_unlock(&locker_hash[hash_val % 128]); return 0;}
    curr=aggiungoroba;
    for(i=0; i<n; i++){
        if(curr->tmess==TXT_MESSAGE)
            num_txt++;
        setHeader(&m.hdr,curr->tmess,curr->sender);
        setData(&m.data,"",curr->mess,curr->lenm);
        sendRequest(connfd,&m);
        curr=curr->next;;
    }
    pthread_mutex_unlock(&locker_hash[hash_val % 128]);
    return num_txt;
}

/**
 * Inserisco nella parte dati di ogni utente registrato un messaggio testuale
 *
 * @param ht -- la tabella hash
 * @param toinsert -- il messaggio
 * @param len -- la lunghezza del messaggio
 * @param sender -- il mittente
 * @param maxin -- il numero max di messaggi di cui ogni utente tiene traccia
 *
 * @returns il num di messaggi inviati all'utente
 */

void inviamessatutti(icl_hash_t * ht,char * toinsert,int len,char* sender,int maxin){
    icl_entry_t * curr;
    mess_t * messcurr;
    mess_t *messcurrfrst;
    for (int i = 0; i < ht->nbuckets; ++i){
        pthread_mutex_lock(&locker_hash[i % 128]);
        curr=ht->buckets[i];
        while(curr!=NULL){
            if(strcmp(curr->key,sender)!=0){
       //         printf("sto inviando a %s\n",(char *)curr->key );
                messcurr=(mess_t *)curr->data;
                messcurrfrst=(mess_t *)curr->data;
                if(curr->numess==0){
                    messcurr->tmess=TXT_MESSAGE;
                    messcurr->lenm=len;
                    messcurr->mess=malloc(sizeof(char)*len);
                    strcpy(messcurr->mess,toinsert);
                    messcurr->sender=malloc(33*sizeof(char));
                    strcpy(messcurr->sender,sender);
                    messcurr->next=NULL;
                    curr->numess=1;
                }
                else{
                    while(messcurr->next!=NULL)
                        messcurr=messcurr->next;
                    mess_t * elemento=malloc(sizeof(mess_t));
                    elemento->tmess=TXT_MESSAGE;
                    elemento->lenm=len;
                    elemento->mess=malloc(sizeof(char)*len);
                    strcpy(elemento->mess,toinsert);
                    elemento->sender=malloc(33*sizeof(char));
                    strcpy(elemento->sender,sender);
                    elemento->next=NULL;
                    messcurr->next=elemento;
                    curr->numess++;
                    if(curr->numess>maxin){ 
                      curr->data=(mess_t *)messcurrfrst->next;
                      free(messcurrfrst->sender);
                      messcurrfrst->sender=NULL;
                      free(messcurrfrst->mess);
                      messcurrfrst->mess=NULL;
                      free(messcurrfrst);
                      messcurrfrst=NULL;
                      curr->numess--;
                    }
                }
            }
            curr=curr->next;
        }
        pthread_mutex_unlock(&locker_hash[i % 128]);
    }
}