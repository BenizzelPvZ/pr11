# Dateikopier-Protokoll für Unix-Domain-Socket

## Übersicht

Kommunikation zwischen Client und Server über Unix-Domain-Socket `/tmp/mysocket`.
Der Server läuft iterativ und akzeptiert sequentiell Verbindungen.

## Message-Typen

| Typ | Richtung | Beschreibung | Format |
|-----|----------|--------------|--------|
| FILENAME | Client → Server | Übermittelt den Zieldateinamen | `FILENAME:<name>\n` |
| ACK | Server → Client | Bestätigung | `ACK\n` |
| NAK | Server → Client | Ablehnung mit Grund | `NAK:<grund>\n` |
| FILESIZE | Client → Server | Dateigröße in Bytes | `FILESIZE:<größe>\n` |
| FILEDATA | Client → Server | Dateiblock | `FILEDATA:<länge>\n<daten>` (genau <länge> Bytes nach Header) |
| ERROR | Beide | Fehlernachricht | `ERROR:<nachricht>\n` |
| DONE | Client → Server | Übertragung abgeschlossen | `DONE\n` |

## Ablauf der Übertragung

```
1. Client → Server: FILENAME:<Zieldateiname>\n
2. Server prüft, ob Zieldatei existiert:
   - Falls JA: Server → Client: NAK:File already exists\n
   - Falls NEIN: Server → Client: ACK\n

3. Client prüft Quelldatei:
   - Falls nicht existent/nicht lesbar: Client → Server: ERROR:<grund>\n → Verbindung schließen

4. Client → Server: FILESIZE:<Größe>\n
5. Server → Client: ACK\n

6. Client sendet Datei in Blöcken (max. 1024 Bytes):
   - Client → Server: FILEDATA:<Länge>\n<Datenblock> (Länge = tatsächliche Blockgröße)
   - Server → Client: ACK\n (nach jedem Block)
   - Wiederhole bis gesamte Datei übertragen

7. Client → Server: DONE\n
8. Server → Client: ACK\n

9. Verbindung wird geschlossen
```

## Fehlerbehandlung

### Client-Fehler (vor Übertragung)
- Quelldatei existiert nicht: `ERROR:Source file does not exist\n`
- Quelldatei nicht lesbar: `ERROR:Cannot read source file\n`
- Keine Leserechte: `ERROR:Permission denied on source file\n`

### Server-Fehler
- Zieldatei existiert bereits: `NAK:File already exists\n` (vor Übertragungsbeginn)
- Zieldatei nicht erstellbar: `ERROR:Cannot create destination file\n`
- Schreibfehler: `ERROR:Write error\n`
- Unbekannter Message-Typ: `ERROR:Invalid protocol message\n`

## Socket-Cleanup

- Server registriert `atexit`-Handler zum Löschen des Sockets
- Server fängt `SIGINT` (^C) ab und löscht Socket vor Beenden
- Socket-Datei `/tmp/mysocket` wird entfernt

## Blockgröße

- Maximale Blockgröße: 1024 Bytes
- Letzter Block kann kleiner sein
- Jeder Block wird mit `FILEDATA:<Länge>\n` eingeleitet
