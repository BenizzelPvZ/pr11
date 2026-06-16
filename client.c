#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include "common.h"

int send_message(int sock, const char *message);
int receive_message(int sock, char *buffer, size_t bufsize);
int send_exact(int sock, unsigned const char *message, size_t length);
int receive_exact(int sock, void *buf, size_t length);

int main(int argc, char *argv[]) {
    int sock;
    struct sockaddr_un addr;
    int rc;
    char buffer[MAX_MSG_SIZE];
    
    /* Parameter prüfen */
    if (argc != 3) {
        fprintf(stderr, "Aufruf: %s <Quelldatei> <Zieldatei>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    const char *source_filename = argv[1];
    char target_filename[PATH_MAX];
    memset(target_filename,0,PATH_MAX);

    if (argv[2][0] != '/') {
      // Wenn es kein absoluter Pfad ist, holen wir das aktuelle Verzeichnis des Clients
        if (getcwd(target_filename, PATH_MAX-1-1-strlen(argv[2])) == NULL){ //-1 fuer / und -laenge von argv[2] und -1 fuer \0
            perror("getcwd");
            exit(EXIT_FAILURE);
        }; 
        strcat(target_filename, "/");
        strcat(target_filename, argv[2]);
    } else {
        strncpy(target_filename, argv[2], PATH_MAX);
    }
    
    /* 1. Quelldatei prüfen */
    int source_fd = open(source_filename, O_RDONLY);
    if (source_fd == -1) {
        perror("open");
        exit(EXIT_FAILURE);
    }
    
    /* Dateigröße bestimmen */
    off_t file_size = lseek(source_fd, 0, SEEK_END);
    if (file_size < 0) {
        perror("lseek");
        close(source_fd);
        exit(EXIT_FAILURE);
    }
    
    if (lseek(source_fd, 0, SEEK_SET) == -1) {
        perror("lseek");
        close(source_fd);
        exit(EXIT_FAILURE);
    }
    
    
    
    printf("Kopiere '%s' (%lld Bytes) nach '%s'...\n", source_filename, (long long)file_size, target_filename);
    
    /* 2. Verbindung zum Server herstellen */
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket");
        close(source_fd);
        exit(EXIT_FAILURE);
    }
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    rc = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    if (rc == -1) {
        perror("connect");
        fprintf(stderr, "Hinweis: Ist der Server gestartet?\n");
        close(sock);
        close(source_fd);
        exit(EXIT_FAILURE);
    }
    
    printf("Verbunden mit Server\n");
    
    /* 3. Sende Zieldateinamen */
    snprintf(buffer, MAX_MSG_SIZE, "FILENAME:%.*s\n", MAX_MSG_SIZE-(int)sizeof("FILENAME:\n"),target_filename);
    if (send_message(sock, buffer) != 0) {
        close(sock);
        close(source_fd);
        exit(EXIT_FAILURE);
    }
    
    /* 4. Warte auf Antwort (ACK oder NAK oder ERROR) */
    rc = receive_message(sock, buffer, MAX_MSG_SIZE);
    if (rc <= 0) {
        fprintf(stderr, "Server hat Verbindung geschlossen\n");
        close(sock);
        close(source_fd);
        exit(EXIT_FAILURE);
    }
    
    // Newline entfernen
    char *newline = strchr(buffer, '\n');
    if (newline) {
        *newline = '\0';
    }
    
    if (strncmp(buffer, "NAK:", 4) == 0) {
        fprintf(stderr, "Server: %s\n", buffer);
        close(sock);
        close(source_fd);
        exit(EXIT_FAILURE);
    } else if (strncmp(buffer, "ERROR:", 6) == 0) {
        fprintf(stderr, "Server-Fehler: %s\n", buffer);
        close(sock);
        close(source_fd);
        exit(EXIT_FAILURE);
    } else if (strcmp(buffer, "ACK") != 0) {
        fprintf(stderr, "Protokollfehler: Erwartet ACK, erhalten: %s\n", buffer);
        close(sock);
        close(source_fd);
        exit(EXIT_FAILURE);
    }
    
    printf("Server hat Zieldateinamen akzeptiert\n");
    
    /* 5. Sende Dateigröße */
    snprintf(buffer, MAX_MSG_SIZE, "FILESIZE:%lld\n", (long long)file_size);
    if (send_message(sock, buffer) != 0) {
        close(sock);
        close(source_fd);
        exit(EXIT_FAILURE);
    }
    
    /* 6. Warte auf ACK */
    rc = receive_message(sock, buffer, MAX_MSG_SIZE);
    if (rc <= 0) {
        fprintf(stderr, "Server hat Verbindung geschlossen (nach FILESIZE)\n");
        close(sock);
        close(source_fd);
        exit(EXIT_FAILURE);
    }
    
    newline = strchr(buffer, '\n');
    if (newline) {
        *newline = '\0';
    }
    
    if (strcmp(buffer, "ACK") != 0) {
        fprintf(stderr, "Protokollfehler: Erwartet ACK nach FILESIZE, erhalten: %s\n", buffer);
        close(sock);
        close(source_fd);
        exit(EXIT_FAILURE);
    }
    
    /* 7. Sende Datei in Blöcken */
    unsigned char data_block[BLOCK_SIZE];
    ssize_t bytes_read;
    off_t bytes_sent = 0;
    
    while (bytes_sent < file_size) {
        bytes_read = read(source_fd, data_block, BLOCK_SIZE);
        if (bytes_read == 0) {
            // EOF erreicht
            break;
        }
        if (bytes_read == -1) {
            perror("read");
            close(sock);
            close(source_fd);
            exit(EXIT_FAILURE);
        }
        
        /* Sende FILEDATA-Header */
        snprintf(buffer, MAX_MSG_SIZE, "FILEDATA:%zd\n", bytes_read);
        if (send_message(sock, buffer) != 0) {
            close(sock);
            close(source_fd);
            exit(EXIT_FAILURE);
        }
        
        /* Sende Daten */
        if (send_exact(sock, data_block, bytes_read) != (int)bytes_read) {
            fprintf(stderr, "Fehler beim Senden der Daten\n");
            close(sock);
            close(source_fd);
            exit(EXIT_FAILURE);
        }
        
        bytes_sent += bytes_read;
        
        /* Warte auf ACK */
        rc = receive_message(sock, buffer, MAX_MSG_SIZE);
        if (rc <= 0) {
            fprintf(stderr, "Server hat Verbindung geschlossen (während Dateiübertragung)\n");
            close(sock);
            close(source_fd);
            exit(EXIT_FAILURE);
        }
        
        newline = strchr(buffer, '\n');
        if (newline) {
            *newline = '\0';
        }
        
        if (strncmp(buffer, "ERROR:", 6) == 0) {
            fprintf(stderr, "Server-Fehler: %s\n", buffer);
            close(sock);
            close(source_fd);
            exit(EXIT_FAILURE);
        } else if (strcmp(buffer, "ACK") != 0) {
            fprintf(stderr, "Protokollfehler: Erwartet ACK nach Datenblock, erhalten: %s\n", buffer);
            close(sock);
            close(source_fd);
            exit(EXIT_FAILURE);
        }
        
        printf("\rFortschritt: %lld/%lld Bytes (%d%%) ", (long long)bytes_sent, (long long)file_size, 
               (int)((bytes_sent * 100) / file_size));
        fflush(stdout);
        //sleep(1); // für Demozwecke
    }
    
    printf("\n");
    
    /* 8. Sende DONE */
    if (send_message(sock, "DONE\n") != 0) {
        close(sock);
        close(source_fd);
        exit(EXIT_FAILURE);
    }
    
    /* 9. Warte auf finale ACK */
    rc = receive_message(sock, buffer, MAX_MSG_SIZE);
    if (rc <= 0) {
        fprintf(stderr, "Server hat Verbindung geschlossen (nach DONE)\n");
        close(sock);
        close(source_fd);
        exit(EXIT_FAILURE);
    }
    
    newline = strchr(buffer, '\n');
    if (newline) {
        *newline = '\0';
    }
    
    if (strncmp(buffer, "ERROR:", 6) == 0) {
        fprintf(stderr, "Server-Fehler: %s\n", buffer);
        close(sock);
        close(source_fd);
        exit(EXIT_FAILURE);
    } else if (strcmp(buffer, "ACK") != 0) {
        fprintf(stderr, "Protokollfehler: Erwartet ACK nach DONE, erhalten: %s\n", buffer);
        close(sock);
        close(source_fd);
        exit(EXIT_FAILURE);
    }
    
    printf("Datei erfolgreich kopiert!\n");
    
    close(sock);
    close(source_fd);
    
    return EXIT_SUCCESS;
}


/* Sendet eine Nachricht über den Socket */
int send_message(int sock, const char *message) {
    size_t len = strlen(message);
    ssize_t sent = send(sock, message, len, 0);
    if (sent < 0) {
        perror("send");
        return -1;
    }
    return 0;
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
                return 0;
            }
            perror("recv");
            return -1;
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

/* Sendet genau 'length' Bytes */
int send_exact(int sock, unsigned const char *message, size_t length) {
    size_t sent_total = 0;
    
    while (sent_total < length) {
        ssize_t sent = send(sock, message + sent_total, length - sent_total, 0);
        if (sent <= 0) {
            perror("send");
            return -1;
        }
        sent_total += sent;
    }
    
    return sent_total;
}


