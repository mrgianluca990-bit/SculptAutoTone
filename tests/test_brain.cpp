#include "../Source/ToneBrain.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

int main() {
    ToneBrain brain; brain.reset(48000);
    std::vector<float> audio(480, 0.0f);
    std::vector<float> drive {0.08f, 0.35f, 0.72f, 0.95f};
    std::vector<int> votes (4, 0);
    // Pure silence cannot trigger profile switching.
    for (int i = 0; i < 1100; ++i) {
        auto r = brain.update(audio.data(), (int)audio.size(), drive, votes, 0, false);
        assert(r.index == -1);
    }
    // Playing triggers automatic selection after stabilization.
    bool selected = false;
    for (int t = 0; t < 1300; ++t) {
        for (int i = 0; i < (int)audio.size(); ++i)
            audio[i] = 0.15f * std::sin(2.0 * 3.141592653589793 * 220.0 * (t * 480 + i) / 48000.0);
        auto r = brain.update(audio.data(), (int)audio.size(), drive, votes, 0, false);
        if (r.index >= 0) selected = true;
    }
    assert(selected);
    // LOCK blocks any subsequent changes.
    for (int t = 0; t < 1100; ++t) {
        auto r = brain.update(audio.data(), (int)audio.size(), drive, votes, 0, true);
        assert(r.index == -1);
    }
    std::cout << "ToneBrain: silence, selection, LOCK tests passed.\n";
}
