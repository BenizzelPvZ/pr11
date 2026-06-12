#!/usr/bin/env python3
"""
Iterativer Dateikopier-Server für Unix-Domain-Sockets

Verwendet Socket: /tmp/mysocket
Protokoll: Siehe PROTOKOLL.md
"""

import os
import sys
import signal
import socket
import atexit
import errno

SOCKET_PATH = "/tmp/mysocket"
BLOCK_SIZE = 1024
MAX_MESSAGE_SIZE = 4096

# Globaler Socket für Cleanup
server_socket = None


def cleanup_socket():
    """Löscht den Server-Socket und die zugehörige Datei."""
    global server_socket
    if server_socket:
        try:
            server_socket.close()
        except Exception:
            pass
        server_socket = None
    
    # Socket-Datei entfernen
    try:
        os.unlink(SOCKET_PATH)
    except FileNotFoundError:
        pass
    except Exception as e:
        print(f"Warnung: Socket-Datei konnte nicht gelöscht werden: {e}", file=sys.stderr)


def signal_handler(signum, frame):
    """Signal-Handler für SIGINT (^C)."""
    print(f"\nSignal {signum} empfangen. Beende Server...")
    cleanup_socket()
    sys.exit(0)


def send_message(sock, message):
    """Sendet eine Nachricht über den Socket."""
    try:
        sock.sendall(message.encode('utf-8'))
    except Exception as e:
        print(f"Fehler beim Senden: {e}", file=sys.stderr)
        raise


def receive_message(sock):
    """Empfängt eine Nachricht vom Socket (blockierend).
    
    Returns:
        str: Die empfangene Nachricht ohne abschließendes Newline
        oder None bei Fehler/Verbindungsschluss
    """
    try:
        data = sock.recv(MAX_MESSAGE_SIZE)
        if not data:
            return None
        return data.decode('utf-8').rstrip('\n')
    except ConnectionResetError:
        return None
    except Exception as e:
        print(f"Fehler beim Empfangen: {e}", file=sys.stderr)
        raise


def receive_exact(sock, length):
    """Empfängt genau 'length' Bytes vom Socket.
    
    Returns:
        bytes: Die empfangenen Daten
        oder None bei Fehler
    """
    data = b''
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def handle_client(conn, addr):
    """Behandelt eine Client-Verbindung."""
    print(f"Neue Verbindung von {addr}")
    
    try:
        # 1. Warte auf FILENAME-Nachricht
        message = receive_message(conn)
        if not message:
            print("Client hat Verbindung geschlossen (vor FILENAME)")
            return
        
        if not message.startswith("FILENAME:"):
            send_message(conn, f"ERROR:Erwartet FILENAME, erhalten: {message.split(':')[0] if ':' in message else message}\n")
            return
        
        target_filename = message[len("FILENAME:"):]
        print(f"Zieldatei: {target_filename}")
        
        # 2. Prüfe, ob Datei bereits existiert
        if os.path.exists(target_filename):
            send_message(conn, f"NAK:Datei '{target_filename}' existiert bereits\n")
            return
        
        # 3. Datei kann erstellt werden
        send_message(conn, "ACK\n")
        
        # 4. Warte auf FILESIZE
        message = receive_message(conn)
        if not message:
            print("Client hat Verbindung geschlossen (vor FILESIZE)")
            return
        
        if not message.startswith("FILESIZE:"):
            send_message(conn, f"ERROR:Erwartet FILESIZE, erhalten: {message.split(':')[0] if ':' in message else message}\n")
            return
        
        try:
            file_size = int(message[len("FILESIZE:"):])
        except ValueError:
            send_message(conn, "ERROR:Ungültige Dateigröße\n")
            return
        
        print(f"Dateigröße: {file_size} Bytes")
        send_message(conn, "ACK\n")
        
        # 5. Erstelle Zieldatei
        try:
            with open(target_filename, 'wb') as f:
                bytes_received = 0
                
                # Empfange Dateiblöcke
                while bytes_received < file_size:
                    # Warte auf FILEDATA-Header
                    message = receive_message(conn)
                    if not message:
                        print("Client hat Verbindung geschlossen (während Dateiübertragung)")
                        # Teilweise geschriebene Datei löschen
                        try:
                            os.remove(target_filename)
                        except Exception:
                            pass
                        return
                    
                    if not message.startswith("FILEDATA:"):
                        if message == "DONE":
                            send_message(conn, "ERROR:DONE erhalten, aber nicht alle Daten empfangen\n")
                            try:
                                os.remove(target_filename)
                            except Exception:
                                pass
                            return
                        send_message(conn, f"ERROR:Erwartet FILEDATA, erhalten: {message.split(':')[0] if ':' in message else message}\n")
                        try:
                            os.remove(target_filename)
                        except Exception:
                            pass
                        return
                    
                    try:
                        data_length = int(message[len("FILEDATA:"):])
                    except ValueError:
                        send_message(conn, "ERROR:Ungültige Datenlänge\n")
                        try:
                            os.remove(target_filename)
                        except Exception:
                            pass
                        return
                    
                    # Empfange die tatsächlichen Daten
                    data = receive_exact(conn, data_length)
                    if data is None or len(data) != data_length:
                        print(f"Fehler: Erwartet {data_length} Bytes, erhalten {len(data) if data else 0}")
                        send_message(conn, "ERROR:Unvollständige Datensendung\n")
                        try:
                            os.remove(target_filename)
                        except Exception:
                            pass
                        return
                    
                    # Schreibe Daten in Datei
                    try:
                        f.write(data)
                        bytes_received += len(data)
                    except Exception as e:
                        print(f"Schreibfehler: {e}")
                        send_message(conn, f"ERROR:Schreibfehler: {str(e)}\n")
                        try:
                            os.remove(target_filename)
                        except Exception:
                            pass
                        return
                    
                    # Bestätige Empfang des Blocks
                    send_message(conn, "ACK\n")
                
                # 6. Warte auf DONE
                message = receive_message(conn)
                if not message:
                    print("Client hat Verbindung geschlossen (vor DONE)")
                    try:
                        os.remove(target_filename)
                    except Exception:
                        pass
                    return
                
                if message != "DONE":
                    send_message(conn, f"ERROR:Erwartet DONE, erhalten: {message}\n")
                    try:
                        os.remove(target_filename)
                    except Exception:
                        pass
                    return
                
                print(f"Datei '{target_filename}' erfolgreich empfangen ({bytes_received} Bytes)")
                send_message(conn, "ACK\n")
        
        except Exception as e:
            print(f"Fehler beim Schreiben der Datei: {e}")
            send_message(conn, f"ERROR:Fehler beim Schreiben: {str(e)}\n")
            # Teilweise geschriebene Datei löschen
            try:
                if os.path.exists(target_filename):
                    os.remove(target_filename)
            except Exception:
                pass
    
    except Exception as e:
        print(f"Allgemeiner Fehler: {e}", file=sys.stderr)
    finally:
        try:
            conn.close()
        except Exception:
            pass
        print("Verbindung geschlossen")


def main():
    """Hauptfunktion des Servers."""
    global server_socket
    
    # Cleanup registrieren
    atexit.register(cleanup_socket)
    
    # Signal-Handler registrieren
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)
    
    # Altes Socket entfernen
    cleanup_socket()
    
    # Socket erstellen
    try:
        server_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server_socket.bind(SOCKET_PATH)
        server_socket.listen(5)
        print(f"Server gestartet. Warte auf Verbindungen an {SOCKET_PATH}")
        print("Drücke ^C zum Beenden...")
        
        # Iterativer Server - akzeptiert Verbindungen nacheinander
        while True:
            try:
                conn, addr = server_socket.accept()
                handle_client(conn, addr)
            except KeyboardInterrupt:
                raise
            except Exception as e:
                print(f"Fehler bei accept: {e}", file=sys.stderr)
                continue
    
    except Exception as e:
        print(f"Fehler beim Starten des Servers: {e}", file=sys.stderr)
        cleanup_socket()
        sys.exit(1)


if __name__ == "__main__":
    main()
