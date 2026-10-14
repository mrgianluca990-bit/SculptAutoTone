SCULPT GUITAR RIG v1.3.5 - GESTIONE CANCELLAZIONE PRESET

NUOVE FUNZIONI
1. "ELIMINA" in alto sotto la lista preset: apre un elenco dei preset su disco.
   Scegliere dall'elenco di eliminazione NON carica il preset.
   La finestra di conferma presenta "ELIMINA" e "ANNULLA".
   Il preset e' eliminato solo premendo "ELIMINA".
2. Ciascuno dei cinque slot rapidi ha una piccola "X" per svuotarlo,
   visibile soltanto se lo slot contiene una memoria. E' richiesta conferma.
3. Eliminando un preset normale NON si eliminano gli slot rapidi.
   Svuotando uno slot rapido NON si cancella il preset normale associato.
4. La catena DSP attiva NON viene modificata dalle operazioni di eliminazione.
   L'eventuale nome di un preset cancellato viene deselezionato.
5. In caso di errore di scrittura, il messaggio compare nella barra di stato.

NOTA: eliminazione dal disco; l'operazione non e' annullabile dall'app.

AGGIORNAMENTO GITHUB
- Sostituire Source/Main.cpp con quello incluso.
- Sostituire CMakeLists.txt con quello incluso (versione 1.3.5).
- Assets e altri sorgenti rimangono invariati.
- Conservare un solo workflow valido macOS (build-macos.yml con
  riferimento a "Sculpt Guitar Rig.app").

COMPILAZIONE
Il codice e' pronto per la build GitHub Actions. Non e' stata eseguita una
compilazione macOS in questo ambiente: verificare la build prima dell'uso live.
