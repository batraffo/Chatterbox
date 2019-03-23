#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _PARSING_H
#define _PARSING_H
 
char * parsastringa(char * stringa, FILE *configurazione); //parsa la stringa associata alla stringa stringa dal file configurazione

int parsanumero(char * stringa, FILE * configurazione); //parsa il numero assocciato alla stringa stringa dal file configurazione

#endif