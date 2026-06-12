FILENAME    Client → Server   Übermittelt den Zieldateinamen   `FILENAME:<name>\n`
ACK         Server → Client   Bestätigung                      `ACK\n`
NAK         Server → Client   Ablehnung mit Grund              `NAK:<grund>\n`
FILESIZE    Client → Server   Dateigröße in Bytes              `FILESIZE:<größe>\n`
FILEDATA    Client → Server   Dateiblock                       `FILEDATA:<länge>\n<daten>` (genau <länge> Bytes nach Header)
ERROR       Beide             Fehlernachricht                  `ERROR:<nachricht>\n`
DONE        Client → Server   Übertragung abgeschlossen        `DONE\n`


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

- Maximale Blockgröße: 1024 Bytes
- Letzter Block kann kleiner sein
- Jeder Block wird mit `FILEDATA:<Länge>\n` eingeleitet
