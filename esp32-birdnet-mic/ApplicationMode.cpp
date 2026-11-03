#include "AppRuntime.h"

bool setDeviceMode(bool recordingMode) {
    if (recordingMode && !recordingModeSupported()) return false;
    if (recordingMode == recording::mode()) return true;
    if (applicationUpdateInProgress() || flashlayout::busy()) return false;
    const bool previousMode = recording::mode();
    const bool wasRecording = recording::busy();
    if (!recording::stop() || (wasRecording && recording::status().state == recording::State::Error)) return false;
    cancelRecordingAutoStart();
    if (!recordingMode) cancelThermalResume();
    browserAudioStop();
    stopAllRtspClients("Device mode changed");
    rtspServerEnabled = false;
    rtspServer.stop();
    // Quiesce the producer before changing/freeing either mode's buffers.
    // Listen has an independent queue and capture restarts in both modes.
    stopAudioProducer();
    bool ok = recordingMode || recording::releaseBuffer();
    if (ok) {
        recording::setRuntimeMode(recordingMode);
        ok = webui_logModeChanged(recordingMode);
    }
    if (ok && !overheatLatched) ok = startAudioProducer();
    if (!ok) {
        stopAudioProducer();
        recording::setRuntimeMode(previousMode);
        webui_logModeChanged(previousMode);
        if (!overheatLatched && !startAudioProducer())
            simplePrintln("Mode change rollback: audio restart failed.");
    }
    devicesetup::currentMode(recording::mode());
    if (ok && recordingMode && !overheatLatched)
        recording::checkCard(recordingHardwareSupported(), s3BoardVariant);
    if (!recording::mode() && !overheatLatched && isStreamScheduleAllowedNow()) {
        rtspServer.begin(); rtspServer.setNoDelay(true); rtspServerEnabled = true;
    }
    return ok;
}
