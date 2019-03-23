/* Autore: Raffaele Ariano 530519
 * Il programma e', in ogni sua parte, opera originale dell'autore*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <parsing.h>

#define _POSIX_C_SOURCE 200112L


/**
 * Parso la stringa "stringa" dal file "configurazione"
 *
 */

char * parsastringa(char * stringa, FILE *configurazione){
	char *s=malloc(1024*sizeof(char));
	int scorritore;
	char * salvataggio=NULL;
	int scorrinume;
	char *nome;
	char *parsante=malloc(101*sizeof(char));
	while(fgets(s,1024,configurazione)!=NULL){
		scorritore=0;
    	scorrinume=0;
		if(s[0]!='#'){
        	if(s!=NULL){
      			nome=strtok_r(s," ",&salvataggio);
      			if(nome!=NULL){
					if(nome[strlen(nome)-1]=='\t')
	  					nome[strlen(nome)-1]=nome[strlen(nome)];
	  				if(strcmp(nome,stringa)==0){
	  					while(s[scorritore]!= '=')
	    					scorritore++;
	  						scorritore++;
	  					while(s[scorritore]==' ')
	    					scorritore++;
	  					while(s[scorritore]!=' ' && s[scorritore]!='\0'){
	    					parsante[scorrinume]=s[scorritore];
	    					scorritore++;
	    					scorrinume++;
						}
						parsante[scorrinume]='\0';
						if(parsante[strlen(parsante)-1]=='\n')
  							parsante[strlen(parsante)-1]='\0';
  						fseek(configurazione,0,SEEK_SET);
  						free(s);
						return parsante;
					}
				}
			}
		}
	}
	printf("%s non trovata\n", stringa);
	free(s);
	free(parsante);
	return NULL;
}

/**
 * Parso la stringa "stringa", a cui e' associato un intero, dal file "configurazione"
 *
 */

int parsanumero(char * stringa, FILE * configurazione){
	char *s=malloc(1024*sizeof(char));
	int scorritore;
	char * salvataggio=NULL;
	int scorrinume;
	char *nome;
	int numero;
	char *parsante=malloc(101*sizeof(char));
	while(fgets(s,1024,configurazione)!=NULL){
		scorritore=0;
    	scorrinume=0;
		if(s[0]!='#'){
        	if(s!=NULL){
      			nome=strtok_r(s," ",&salvataggio);
      			if(nome!=NULL){
					if(nome[strlen(nome)-1]=='\t')
	  					nome[strlen(nome)-1]=nome[strlen(nome)];
	  				if(strcmp(nome,stringa)==0){
	  					while(s[scorritore]!= '=')
	    					scorritore++;
	  						scorritore++;
	  					while(s[scorritore]==' ')
	    					scorritore++;
	  					while(s[scorritore]!=' ' && s[scorritore]!='\0'){
	    					parsante[scorrinume]=s[scorritore];
	    					scorritore++;
	    					scorrinume++;
						}
						parsante[scorrinume]='\0';
						fseek(configurazione,0,SEEK_SET);
  						numero=atoi(parsante);
  						free(s);
  						free(parsante);
						return numero;
					}
				}
			}
		}
	}
	printf("%s non trovata\n", stringa);
	return -1;
}