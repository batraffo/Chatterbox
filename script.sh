#!/bin/bash

if [ $# -lt 1 ]; then
    echo -e "Codesto programma prende come argomenti il file di configurazione e un intero t\nDal file di configurazione prendo il path dove vengono memorizzati i files da inviare agli utenti e cancello quelli più vecchi di t minuti\nSe t è uguale a 0 allora vedrai il contenuto della cartella\nSe ti piace questo messaggio e vorresti rivederlo basta che tra gli argomenti del programma inserisci -help"
    exit 1
fi

controllo=0
t=0


if [ "$1" = "-help" ]; then
    echo -e "Codesto programma prende come argomenti il file di configurazione e un intero t\nDal file di configurazione prendo il path dove vengono memorizzati i files da inviare agli utenti e cancello quelli più vecchi di t minuti\nSe t è uguale a 0 allora vedrai il contenuto della cartella"
    controllo=1
fi

#se il primo elemento non è -help allora sarà la directory del file di configurazione

if [ $controllo -eq 0 ]; then
    if [ ! -f $1 ]; then #controllo se il file è un file o ho passato altro per sbagolio
	echo $1 is not a file
	exit 1	
    else
	configurazione="$1"
    fi
fi

if [ $controllo -eq 1 ]; then
    if [ ! -f $2 ]; then #controllo se il file è un file o ho passato altro per sbagolio
	echo $2 is not a file
	exit 1
    else
	configurazione="$2"
    fi
fi
    
if [ "$2" = "-help" ]; then
    echo -e "Codesto programma prende come argomenti il file di configurazione e un intero t\nDal file di configurazione prendo il path dove vengono memorizzati i files da inviare agli utenti e cancello quelli più vecchi di t minuti\nSe t è uguale a 0 allora vedrai il contenuto della cartella"
    controllo=1
fi

if [ $controllo -eq 0 ]; then
    if [ "$2" -eq "$2" ] 2> /dev/null; then #controllo se l'intero è un intero o ho passato altro per sbagolio
	let t=$2
    else
	echo $2 is not a integer or there is no integer
	exit 1
    fi
fi

if [ $controllo -eq 1 ]; then
    if [ "$3" -eq "$3" ] 2> /dev/null; then #controllo se l'intero è un intero o ho passato altro per sbagolio
	let t=$3
    else
	echo $3 is not a integer or there is no integer
	exit 1
    fi
fi

if [ $controllo -eq 0 ]; then
  if [ "$3" = "-help" ]; then
    echo -e "Codesto programma prende come argomenti il file di configurazione e un intero t\nDal file di configurazione prendo il path dove vengono memorizzati i files da inviare agli utenti e cancello quelli più vecchi di t minuti\nSe t è uguale a 0 allora vedrai il contenuto della cartella"
  fi
fi

if [ $t -lt 0 ]; then
    echo "integer must be >= 0"
    exit 1
fi
#adesso ho che t sono i minuti e configurazione è il file di configurazione
cartella=`grep -v '[#]' $configurazione | grep DirName|cut -d "=" -f 2` #ho preso la riga con DirName senza cancelletto e ci ho estratto qurl che sta a destra del'uguale
cartellasenzaspazi=`echo $cartella`
cd "$cartellasenzaspazi/" #incredibilmente funziona anche se le cartelle hanno gli spazi
if [ $t -eq 0 ]; then #se t è 0 allora vedo soltanto cosa c'è dentro la cartella
    ls
    exit 0
fi

if [ $t -gt 0 ]; then #sennò cancello quello che stava prima di t minuti fa
    find . -mmin +$t -delete
    exit 0
fi

