export interface MeterSnapshot {
  inputLufs: number;
  outputLufs: number;
  inputPeak: number;
  outputPeak: number;
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
