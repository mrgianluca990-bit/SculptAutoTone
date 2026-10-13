SCULPT AUTOTONE v0.6 — VST3 FX RIG (BETA SORGENTI)
===================================================
CATENA (nell'ordine reale del DSP)
INPUT DI -> Noise Gate interno -> PRE 1 -> PRE 2 -> PRE 3 -> PRE 4 -> NAM -> Cabinet IR -> SCULPT CHANNEL VST3 -> POST 1 -> POST 2 -> POST 3 -> POST 4 -> MASTER OUTPUT stereo.

Installazione nel repo github.com/mrgianluca990-bit/SculptAutoTone:
1. Sostituisci CMakeLists.txt nella root con quello di questo pacchetto.
2. Nella cartella Source/ carica i file Main.cpp e VstChain.h di questo pacchetto.
   ToneBrain.h resta invariato.
3. Esegui Commit su main. Il workflow macOS esistente tentera' una nuova compilazione.
4. Scarica la .app dall'artifact della build solo se l'esito e' verde.

INTERFACCIA
- Apri l'app e premi "APRI RIG / VST3 PRE · SCULPT CH · POST".
- Quattro slot PRE e quattro POST indipendenti. Per ciascuno: CARICA VST3, EDIT, ON/BYPASS, X.
- Slot SCULPT CHANNEL dedicato dopo cabinet IR: carica l'effetto VST3 Sculpt Channel installato sul Mac. Non e' una versione DSP incorporata.
- GATE fisso prima di PRE 1, con ON/OFF e soglia in dB.
- I VST3 sono rilevati in /Library/Audio/Plug-Ins/VST3 e ~/Library/Audio/Plug-Ins/VST3.
- La selezione dei VST3 usa il caricamento dei plugin installati su questo Mac. L'app non installa plugin.

LIMITI DELLA PRIMA BETA
- Il codice NON e' ancora stato compilato per macOS. Verificare GitHub Actions e log.
- Nessuna sandbox/separazione di processo: un VST3 instabile potrebbe mandare in crash AutoTone.
- Nessuna memorizzazione persistente delle catene all'uscita; ogni riavvio ha slot vuoti.
- Nessuna gestione esplicita di tempo/trasporto/PDC per i plugin VST3 sync al tempo.
- Un plugin senza build per l'architettura corrente potrebbe non caricarsi.
- Il comportamento audio effettivo si dovra' collaudare con una chitarra e una scheda audio.
- Il segnale PRE e' ricondotto a mono prima del NAM; il segnale post IR/VST3 resta stereo.
- La selezione automatica del NAM analizza il segnale DI originale.

Non e' un plugin AAX e non e' un host VST3 garantito per ogni plugin commerciale.
