Sculpt Guitar Rig v1.3.4 - GUI Preview Fix (source)

PER AGGIORNARE LA REPOSITORY:
- Sostituisci Source/Main.cpp con quello in questa cartella.
- Sostituisci CMakeLists.txt per aggiornare la versione.
- Mantieni Assets/RigArtwork.png e Assets/AppIcon.png gia esistenti.
- Mantieni UN SOLO workflow valido .github/workflows/build-macos-corretto.yml oppure
  .github/workflows/build-macos.yml, NON entrambi: il vecchio workflow genera errori.

CAMBIAMENTI:
* Stringhe UI ASCII, eliminati i simboli Unicode incompatibili.
* Meter input MONO (livello DI reale pre-effetti).
* Meter output L/R indipendenti (post-effetti e master).
* Slot vuoti: piastra di copertura metallica (tappo).
* Slot con plugin: faceplate grafico basato sul nome reale.
* Dopo aver aperto e chiuso l'editor tramite EDIT: snapshot statico in RAM del plugin.
  L'editor non resta embedded nel rack; alcuni editor grafici nativi/GPU possono
  non essere acquisibili, nel qual caso la GUI conserva il fallback grafico.
* Snapshot cancellati quando cambia plugin o si richiama un preset.
* Viti, griglie, dettagli metallici, scale attorno alle manopole.
* Nessuna modifica alla catena audio o alle funzioni di salvataggio preset.

NOTA: Questa versione e pronta da compilare, non e ancora verificata con una build macOS.
