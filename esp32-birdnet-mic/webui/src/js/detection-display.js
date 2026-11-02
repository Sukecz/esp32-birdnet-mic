// Render confirmed runtime values, never the unsaved selection in a form.
// Shared by Audio and Device configuration; keep signal format distinct from model.
function detectionDisplay(state){
 const d=state.detection||{},board=Number(state.s3_board??0),format=Number(state.mic_format??0);
 const boardName=d.board_name||state.board_name||['XIAO ESP32-S3','XIAO ESP32-S3 Sense','M5Stack AtomS3U'][board]||'Unknown board';
 const formatName=['Philips I2S','SPH0645 (MSB I2S)','PDM'][format]||'Unknown format';
 const boardConfirmed=d.board_auto&&Number.isInteger(d.detected_board)&&d.detected_board===board;
 const micConfirmed=d.mic_auto&&Number.isInteger(d.detected_format)&&d.detected_format===format;
 return {
  board:boardConfirmed?`Detected: ${boardName}.`:d.board_auto?`Using: ${boardName}. Automatic identification not confirmed; previous configuration kept.`:`Using: ${boardName} (manual selection).`,
  microphone:micConfirmed?`Detected: ${formatName}.`:d.mic_auto?`No microphone confirmed. Keeping: ${formatName}.`:`Using: ${formatName} (manual selection).`
 };
}
