/**
 * Iterativer Dateikopier-Server für Unix-Domain-Sockets
 * 
 * Verwendet Socket: /tmp/mysocket
 * Protokoll: Siehe PROTOKOLL.md
 * 
 * Compilieren: gcc server.c -o server
 * Ausführen: ./server
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include "common.h"

void handle_client(int conn_socket);
void exithandler();
void sighandler(int);
int send_message(int sock, const char *message);
int receive_message(int sock, char *buffer, size_t bufsize);
int receive_exact(int sock, void *buf, size_t length);

int server_socket = -1;

int main() {
    struct sockaddr_un addr;
    struct sigaction old, new;
    int rc;
    
    /* atexit-Handler registrieren */
    atexit(exithandler);
    
    /* Signal-Handler registrieren */
    sigemptyset(&(new.sa_mask));
    new.sa_handler = sighandler;
    new.sa_flags = 0;

    if(sigaction(SIGINT, &new, &old) < 0) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
    
    /* Altes Socket entfernen */
    unlink(SOCKET_PATH);
    
    /* Socket erstellen */
    server_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    /* Socket binden */
    rc = bind(server_socket, (struct sockaddr*)&addr, sizeof(addr));
    if (rc == -1) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    
    /* Socket zum Hören vorbereiten */
    rc = listen(server_socket, 5);
    if (rc == -1) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    
    printf("Server gestartet. Warte auf Verbindungen an %s\n", SOCKET_PATH);
    printf("Drücke ^C zum Beenden...\n");
    
    /* Iterativer Server */
    while (1) {
        struct sockaddr_un client_addr;
        socklen_t client_len = sizeof(client_addr);
        int conn_socket;
        
        conn_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (conn_socket == -1) {
            if (errno == EINTR) {
                // Unterbrochen durch Signal - weitermachen
                continue;
            }
            perror("accept");
            continue;
        }
        
        handle_client(conn_socket);
    }
    
    exithandler();
    return EXIT_SUCCESS;
}

/* Socket und Datei aufräumen */
void exithandler()
{
   if(close(server_socket) < 0) {
     perror("close");
     _exit(EXIT_FAILURE);
   }

   if(unlink(SOCKET_PATH) < 0) {
     perror("unlink");
     _exit(EXIT_FAILURE);
   }
   return;
}

void sighandler(int)
{
   exithandler();
   _exit(EXIT_SUCCESS);
}

/* Sendet eine Nachricht über den Socket */
int send_message(int sock, const char *message) {
    size_t len = strlen(message);
    ssize_t sent = send(sock, message, len, 0);
    if (sent < 0) {
        perror("send");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

/* Empfängt eine Nachricht (bis Newline oder MAX_MSG_SIZE) */
int receive_message(int sock, char *buffer, size_t bufsize) {
    ssize_t received;
    size_t total = 0;
    char c;
    
    while (total < bufsize - 1) {
        received = recv(sock, &c, 1, 0);
        if (received <= 0) {
            if (received == 0) {
                // Verbindung geschlossen
                buffer[total] = '\0';
                return EXIT_SUCCESS;
            }
            perror("recv");
            return EXIT_FAILURE;
        }
        
        buffer[total++] = c;
        
        // Newline gefunden - Nachricht vollständig
        if (c == '\n') {
            buffer[total] = '\0';
            return total;
        }
    }
    
    buffer[total] = '\0';
    return total;
}

/* Empfängt genau 'length' Bytes */
int receive_exact(int sock, void *buf, size_t length) {
    unsigned char *ptr = (unsigned char *)buf;
    size_t received_total = 0;
    
    while (received_total < length) {
        ssize_t received = recv(sock, ptr + received_total, length - received_total, 0);
        if (received <= 0) {
            if (received == 0) {
                // Verbindung geschlossen
                return EXIT_FAILURE;
            }
            perror("recv");
            return EXIT_FAILURE;
        }
        received_total += received;
    }
    
    return received_total;
}

/* Behandelt eine Client-Verbindung */
void handle_client(int conn_socket) {
    char msg_buffer[MAX_MSG_SIZE];
    char error_msg[MAX_MSG_SIZE];
    char filename_buffer[MAX_MSG_SIZE];
    char *newline = NULL;
    int rc;
    char *target_filename = NULL;
    int dest_fd = -1;
    char data_block[BLOCK_SIZE];
    off_t file_size = 0;
    off_t bytes_received = 0;
    
    printf("Neue Verbindung\n");
    
    /* 1. Warte auf FILENAME-Nachricht */
    rc = receive_message(conn_socket, msg_buffer, MAX_MSG_SIZE);
    if (rc <= 0) {
        printf("Client hat Verbindung geschlossen (vor FILENAME)\n");
        goto cleanup;
    }
    
    if (strncmp(msg_buffer, "FILENAME:", 9) != 0) {
        snprintf(error_msg, MAX_MSG_SIZE, "ERROR:Erwartet FILENAME, erhalten: %.*s\n", 
            (int)MAX_MSG_SIZE-(int)sizeof("ERROR:Erwartet FILENAME, erhalten: \n"), msg_buffer);
        send_message(conn_socket, error_msg);
        goto cleanup;
    }
    
    target_filename = msg_buffer + 9; //Prefix ueberspringen
    
    // Newline entfernen
    newline = strchr(target_filename, '\n');
    if (newline) {
        *newline = '\0';
    }
    
    // Kopiere Dateinamen in sicheren Buffer
    if (strncpy(filename_buffer, target_filename, MAX_MSG_SIZE - 1) == NULL) {
        send_message(conn_socket, "ERROR:Kann Dateinamen nicht kopieren\n");
        goto cleanup;
    }

    filename_buffer[MAX_MSG_SIZE - 1] = '\0';
    
    printf("Zieldatei: %s\n", filename_buffer);
    /* 2. Prüfe, ob Datei bereits existiert */
    if (access(target_filename, F_OK) == 0) {
        char nak_msg[MAX_MSG_SIZE];
        snprintf(nak_msg, sizeof(nak_msg), "NAK:Datei '%.*s' existiert bereits\n", (int)strlen(filename_buffer), filename_buffer);
        send_message(conn_socket, nak_msg);
        goto cleanup;
    }
    
    /* 3. Bestätige */
    if (send_message(conn_socket, "ACK\n") != 0) {
        goto cleanup;
    }
    
    /* 4. Warte auf FILESIZE */
    rc = receive_message(conn_socket, msg_buffer, MAX_MSG_SIZE);
    if (rc <= 0) {
        printf("Client hat Verbindung geschlossen (vor FILESIZE)\n");
        goto cleanup;
    }
    if (strncmp(msg_buffer, "FILESIZE:", 9) != 0) {
        snprintf(error_msg, MAX_MSG_SIZE, "ERROR:Erwartet FILESIZE, erhalten: %.*s\n", 
            (int)MAX_MSG_SIZE-(int)sizeof("ERROR:Erwartet FILESIZE, erhalten: \n"), msg_buffer);
        send_message(conn_socket, error_msg);
        goto cleanup;
    }
    char *size_str = msg_buffer + 9; //Prefix ueberspringen
    newline = strchr(size_str, '\n');
    if (newline) {
        *newline = '\0';
    }
    
    errno = 0;
    file_size = atoi(size_str);    //Zahl aus string extrahieren
    if (errno != 0 || file_size < 0) {
        send_message(conn_socket, "ERROR:Ungültige Dateigröße\n");
        goto cleanup;
    }
    
    printf("Dateigröße: %lld Bytes\n", (long long)file_size);
    
    if (send_message(conn_socket, "ACK\n") != 0) {
        goto cleanup;
    }
    
    /* 5. Öffne Zieldatei */
    dest_fd = open(filename_buffer, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (dest_fd == -1) {
        snprintf(msg_buffer, MAX_MSG_SIZE, "ERROR:Cannot create destination file: %s\n", strerror(errno));
        send_message(conn_socket, msg_buffer);
        goto cleanup;
    }
    
    /* 6. Empfange Dateiblöcke bis DONE kommt */
    while (1) {
        /* Warte auf FILEDATA-Header oder DONE */
        rc = receive_message(conn_socket, msg_buffer, MAX_MSG_SIZE);
        if (rc <= 0) {
            printf("Client hat Verbindung geschlossen (während Dateiübertragung)\n");
            goto cleanup_with_file;
        }
        
        // Prüfe ob DONE erhalten wurde
        char msg_copy[MAX_MSG_SIZE];
        strncpy(msg_copy, msg_buffer, MAX_MSG_SIZE - 1);
        msg_copy[MAX_MSG_SIZE - 1] = '\0';
        newline = strchr(msg_copy, '\n');
        if (newline) *newline = '\0';
        
        if (strcmp(msg_copy, "DONE") == 0) {
            // Alle Daten empfangen, DONE erhalten
            break;
        }
        
        if (strncmp(msg_buffer, "FILEDATA:", 9) != 0) {
            char *colon = strchr(msg_buffer, ':');
            size_t prefix_len = colon ? (size_t)(colon - msg_buffer) : strlen(msg_buffer);
            char error_msg2[MAX_MSG_SIZE];
            snprintf(error_msg2, MAX_MSG_SIZE, "ERROR:Erwartet FILEDATA oder DONE, erhalten: %.*s\n",
                     (int)prefix_len, msg_buffer);
            send_message(conn_socket, error_msg2);
            goto cleanup_with_file;
        }
        
        char *length_str = msg_buffer + 9;
        newline = strchr(length_str, '\n');
        if (newline) {
            *newline = '\0';
        }
        
        errno = 0;
        size_t data_length = (size_t)atoi(length_str);
        if (errno != 0 || data_length > BLOCK_SIZE) {
            send_message(conn_socket, "ERROR:Ungültige Datenlänge\n");
            goto cleanup_with_file;
        }
        
        /* Empfange die Daten */
        memset(data_block, 0, sizeof(data_block));

        rc = receive_exact(conn_socket, data_block, data_length);
        if (rc != (int)data_length) {
            send_message(conn_socket, "ERROR:Unvollständige Datensendung\n");
            goto cleanup_with_file;
        }
        
        /* Schreibe Daten in Datei */
        ssize_t written = write(dest_fd, data_block, data_length);
        
        if (written != (ssize_t)data_length) {
            if (written == -1) {
                perror("write");
            }
            send_message(conn_socket, "ERROR:Schreibfehler\n");
            goto cleanup_with_file;
        }
        
        bytes_received += data_length;
        /* Bestätige */
        if (send_message(conn_socket, "ACK\n") < 0) {
            goto cleanup_with_file;
        }
    }
    
    printf("Datei '%s' erfolgreich geschrieben (%lld Bytes)\n", filename_buffer, (long long)bytes_received);
    
    if (send_message(conn_socket, "ACK\n") < 0) {
        goto cleanup_with_file;
    }
    
    goto cleanup;

cleanup_with_file:
    if (dest_fd != -1) {
        if (close(dest_fd) < 0) {
            perror("close, dest_fd");
        }
    }
    dest_fd = -1;
    
    if (unlink(target_filename) < 0) {
        perror("unlink");
    }
    

cleanup:
    if (dest_fd != -1) {
        if (close(dest_fd) < 0) {
            perror("close, dest_fd");
        }
    }

    if(close(conn_socket) < 0) {
        perror("close, conn_socket");
    }
    printf("Verbindung geschlossen\n");
}
