from pathlib import Path
p=Path(__file__).resolve().parent
main=(p/'Source/Main.cpp').read_text()
chain=(p/'Source/VstChain.h').read_text()
cmake=(p/'CMakeLists.txt').read_text()
checks={
 'Macro VST3 attiva': 'JUCE_PLUGINHOST_VST3=1' in cmake,
 'Libreria JUCE host inclusa': 'juce::juce_audio_processors' in cmake,
 '9 slot': 'totalSlots = 9' in chain,
 'Quattro PRE': 'for (int si=0;si<4;++si) vstChain.process(si, vstView)' in main,
 'SCULPT Channel dopo cabinet IR': main.index('vstChain.process(4, vstView)') > main.index('if (irLoaded.load())'),
 'Quattro POST': 'for (int si=5;si<9;++si) vstChain.process(si, vstView)' in main,
 'Volume MASTER dopo i POST': main.index('masterGain.setTargetValue(masterDb') > main.index('for (int si=5;si<9;++si)'),
 'Ingresso DI per algoritmo NAM': 'brain.update(dryInput.data()' in main,
 'Gate prima dei PRE': main.index('const float threshold = gainFromDb(gateThresholdDb') < main.index('for (int si=0;si<4'),
 'VST3 host reale': 'createPluginInstance' in chain and 'processBlock(buffer,noMidi)' in chain,
 'Niente slot FX sintetizzati della v0.5': 'struct MiniFx' not in main,
}
for name,passed in checks.items(): print(('OK   ' if passed else 'FAIL ') + name)
assert all(checks.values())
print('Nota: sono test strutturali, non una build macOS')
