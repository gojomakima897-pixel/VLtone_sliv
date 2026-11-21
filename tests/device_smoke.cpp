// Manual hardware smoke test — NOT part of ctest, since it opens a real audio
// device. Run ./bin/device_smoke by hand to confirm the device layer and the
// graph engine actually produce sound on this machine.
#include "EngineController.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    daw::EngineController controller;
    auto r = controller.initialize(48000, 512, /*openDevice=*/true);
    if (!r) {
        std::printf("FAILED to open the audio device: %s\n", r.message().c_str());
        return 1;
    }
    std::printf("device open: %s, %.0f Hz, %u frames, %u workers\n",
                controller.isDeviceOpen() ? "yes" : "no", controller.sampleRate(),
                controller.bufferSizeFrames(), controller.workerCount());

    const std::string track = controller.addTrack(daw::TrackKind::Audio, "Smoke");
    std::printf("nodes in the compiled graph: %zu\n",
                controller.routingGraph() ? controller.routingGraph()->nodes.size() : 0);

    controller.play();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::printf("position after 2 s: %.2f s, dsp load %.1f%%\n",
                controller.positionSeconds(), controller.dspLoad() * 100.0);
    controller.stop();
    controller.shutdown();
    std::printf("done\n");
    return 0;
}
