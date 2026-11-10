import nativeEngine from 'libmasterme.so';

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

export class MasterMeEngine {
  private handle: Object;

  constructor(sampleRate: number = 48000, bufferSize: number = 512) {
    this.handle = nativeEngine.createEngine(sampleRate, bufferSize);
  }

  loadPreset(index: number): void {
    nativeEngine.loadPreset(this.handle, index);
  }

  setParameter(index: number, value: number): void {
    nativeEngine.setParameter(this.handle, index, value);
  }

  getParameter(index: number): number {
    return nativeEngine.getParameter(this.handle, index);
  }

  processInterleaved(input: Float32Array, output: Float32Array, frameCount: number): MeterSnapshot {
    return nativeEngine.processInterleaved(this.handle, input, output, frameCount);
  }

  getMeterSnapshot(): MeterSnapshot {
    return nativeEngine.getMeterSnapshot(this.handle);
  }

  startAudio(): AudioStatus {
    return nativeEngine.startAudio(this.handle);
  }

  stopAudio(): AudioStatus {
    return nativeEngine.stopAudio(this.handle);
  }

  getAudioStatus(): AudioStatus {
    return nativeEngine.getAudioStatus(this.handle);
  }

  destroy(): void {
    nativeEngine.destroyEngine(this.handle);
  }
}
