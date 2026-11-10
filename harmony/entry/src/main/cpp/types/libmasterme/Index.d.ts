export interface MeterSnapshot {
  inputLufs: number;
  outputLufs: number;
  inputPeak: number;
  outputPeak: number;
}

export interface AudioStatus {
  running: boolean;
  requested: boolean;
  sampleRate: number;
  channels: number;
  callbackFrames: number;
  captureCallbacks: number;
  renderCallbacks: number;
  underruns: number;
  overflows: number;
  deviceChangeCount: number;
  recoveryCount: number;
  errorCount: number;
  lastError: string;
}

export const createEngine: (sampleRate: number, bufferSize: number) => Object;
export const destroyEngine: (handle: Object) => void;
export const loadPreset: (handle: Object, presetIndex: number) => void;
export const setParameter: (handle: Object, parameterIndex: number, value: number) => void;
export const getParameter: (handle: Object, parameterIndex: number) => number;
export const processInterleaved: (
  handle: Object,
  input: Float32Array,
  output: Float32Array,
  frameCount: number
) => MeterSnapshot;
export const getMeterSnapshot: (handle: Object) => MeterSnapshot;
export const startAudio: (handle: Object) => AudioStatus;
export const stopAudio: (handle: Object) => AudioStatus;
export const getAudioStatus: (handle: Object) => AudioStatus;
