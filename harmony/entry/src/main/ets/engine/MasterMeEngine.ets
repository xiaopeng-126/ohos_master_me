import nativeEngine from 'libmasterme.so';

export interface MeterSnapshot {
  inputLufs: number;
  outputLufs: number;
  inputPeak: number;
  outputPeak: number;
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

  destroy(): void {
    nativeEngine.destroyEngine(this.handle);
  }
}
