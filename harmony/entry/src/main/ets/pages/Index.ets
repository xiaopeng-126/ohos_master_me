import { abilityAccessCtrl, Permissions } from '@kit.AbilityKit';
import { AudioStatus, MasterMeEngine, MeterSnapshot } from '../engine/MasterMeEngine';

class Preset {
  name: string;
  subtitle: string;
  target: number;

  constructor(name: string, subtitle: string, target: number) {
    this.name = name;
    this.subtitle = subtitle;
    this.target = target;
  }
}

const PRESETS: Preset[] = [
  new Preset('YOUTUBE', '-14 LUFS', -14),
  new Preset('APPLE PODCASTS', '-16 LUFS', -16),
  new Preset('EBU R128', '-23 LUFS', -23),
  new Preset('SPEECH', '-16 LUFS', -16),
  new Preset('MUSIC', '-16 LUFS', -16)
];

class PARAM {
  static readonly BYPASS: number = 0;
  static readonly TARGET: number = 1;
  static readonly INPUT_GAIN: number = 2;
  static readonly DC_BLOCKER: number = 6;
  static readonly GATE_THRESHOLD: number = 9;
  static readonly EQ_HIGHPASS: number = 14;
  static readonly EQ_TILT: number = 15;
  static readonly LEVELER_SPEED: number = 20;
  static readonly LEVELER_BRAKE: number = 21;
  static readonly COMP_STRENGTH: number = 25;
  static readonly MS_STRENGTH: number = 35;
  static readonly LIMITER_STRENGTH: number = 51;
  static readonly BRICKWALL_CEILING: number = 59;
  static readonly IN_LUFS: number = 63;
  static readonly OUT_LUFS: number = 66;
  static readonly OUT_PEAK_L: number = 67;
  static readonly OUT_PEAK_R: number = 68;
}

@Component
struct LevelBar {
  @Prop value: number = -24;
  @Prop accent: string = '#74B7AE';

  private normalized(): number {
    return Math.max(0, Math.min(1, (this.value + 60) / 60));
  }

  build() {
    Stack({ alignContent: Alignment.Bottom }) {
      Column()
        .width(18)
        .height('100%')
        .borderRadius(4)
        .backgroundColor('#0D0F11')
        .border({ width: 1, color: '#30343A' })

      Column()
        .width(18)
        .height(`${this.normalized() * 100}%`)
        .borderRadius(4)
        .linearGradient({
          direction: GradientDirection.Bottom,
          colors: [[this.accent, 0.0], ['#A15FC2', 0.74], ['#E45A84', 1.0]]
        })
    }
    .width(22)
    .height('100%')
  }
}

@Component
struct ParameterSlider {
  @Prop label: string = '';
  @Prop valueText: string = '';
  @Prop min: number = 0;
  @Prop max: number = 100;
  @Prop value: number = 0;
  onValueChange: (value: number) => void = () => {};

  build() {
    Column({ space: 5 }) {
      Row() {
        Text(this.label)
          .fontSize(12)
          .fontColor('#B8BEC5')
          .fontWeight(FontWeight.Medium)
        Blank()
        Text(this.valueText)
          .fontSize(12)
          .fontColor('#E8F6F3')
          .fontWeight(FontWeight.Medium)
      }
      .width('100%')

      Slider({
        value: this.value,
        min: this.min,
        max: this.max,
        step: 0.1,
        style: SliderStyle.OutSet
      })
        .width('100%')
        .selectedColor('#74B7AE')
        .trackColor('#34383E')
        .blockColor('#F2F5F4')
        .onChange((value: number) => {
          this.onValueChange(value);
        })
    }
    .width('100%')
  }
}

@Entry
@Component
struct Index {
  @State processorEnabled: boolean = true;
  @State expertMode: boolean = false;
  @State selectedPreset: number = 0;
  @State targetLufs: number = -14;
  @State inputGain: number = 0;
  @State gateThreshold: number = -90;
  @State eqTilt: number = 0;
  @State levelerSpeed: number = 50;
  @State compressorStrength: number = 15;
  @State limiterStrength: number = 80;
  @State ceiling: number = -1;
  @State inputLufs: number = -24;
  @State outputLufs: number = -14;
  @State outputPeak: number = -3.2;
  @State engineState: string = 'ENGINE READY';
  @State selfTestPassed: boolean = false;
  @State audioRunning: boolean = false;
  @State audioRequested: boolean = false;
  @State audioStatusText: string = 'LIVE AUDIO OFF';
  @State callbackFrames: number = 0;
  @State underruns: number = 0;
  @State deviceChangeCount: number = 0;

  private engine?: MasterMeEngine = undefined;
  private audioTimer: number = -1;
  private canvasReady: boolean = false;
  private inputHistory: number[] = [];
  private outputHistory: number[] = [];
  private historySettings: RenderingContextSettings = new RenderingContextSettings(true);
  private historyContext: CanvasRenderingContext2D =
    new CanvasRenderingContext2D(this.historySettings);

  aboutToAppear(): void {
    try {
      this.engine = new MasterMeEngine(48000, 512);
      this.applyPreset(0, false);
      this.engineState = 'DSP ONLINE';
      this.audioTimer = setInterval(() => {
        this.refreshAudioStatus();
      }, 250);
    } catch (error) {
      this.engineState = 'DSP OFFLINE';
      this.audioStatusText = 'AUDIO UNAVAILABLE';
    }
  }

  aboutToDisappear(): void {
    if (this.audioTimer >= 0) {
      clearInterval(this.audioTimer);
      this.audioTimer = -1;
    }
    this.engine?.stopAudio();
    this.engine?.destroy();
  }

  private applyPreset(index: number, syncNative: boolean = true): void {
    if (index < 0 || index >= PRESETS.length) {
      return;
    }
    this.selectedPreset = index;
    this.targetLufs = PRESETS[index].target;
    if (syncNative) {
      this.engine?.loadPreset(index);
      this.targetLufs = this.engine?.getParameter(PARAM.TARGET) ?? this.targetLufs;
      this.inputGain = this.engine?.getParameter(PARAM.INPUT_GAIN) ?? this.inputGain;
      this.gateThreshold = this.engine?.getParameter(PARAM.GATE_THRESHOLD) ?? this.gateThreshold;
      this.eqTilt = this.engine?.getParameter(PARAM.EQ_TILT) ?? this.eqTilt;
      this.levelerSpeed = this.engine?.getParameter(PARAM.LEVELER_SPEED) ?? this.levelerSpeed;
      this.compressorStrength = this.engine?.getParameter(PARAM.COMP_STRENGTH) ?? this.compressorStrength;
      this.limiterStrength = this.engine?.getParameter(PARAM.LIMITER_STRENGTH) ?? this.limiterStrength;
      this.ceiling = this.engine?.getParameter(PARAM.BRICKWALL_CEILING) ?? this.ceiling;
    }
  }

  private setParameter(index: number, value: number): void {
    this.engine?.setParameter(index, value);
  }

  private applyAudioStatus(status: AudioStatus): void {
    this.audioRunning = status.running;
    this.audioRequested = status.requested;
    this.callbackFrames = status.callbackFrames;
    this.underruns = status.underruns;
    this.deviceChangeCount = status.deviceChangeCount;
    if (status.lastError.length > 0) {
      this.audioStatusText = status.lastError.toUpperCase();
    } else if (status.running) {
      this.audioStatusText = `LIVE ${status.sampleRate / 1000} kHz / ${status.channels} CH`;
    } else if (status.requested) {
      this.audioStatusText = 'AUDIO STARTING';
    } else {
      this.audioStatusText = 'LIVE AUDIO OFF';
    }
  }

  private refreshAudioStatus(): void {
    if (this.engine === undefined) {
      return;
    }

    try {
      const meters = this.engine.getMeterSnapshot();
      this.inputLufs = meters.inputLufs;
      this.outputLufs = meters.outputLufs;
      this.outputPeak = meters.outputPeak;
      this.applyAudioStatus(this.engine.getAudioStatus());
      this.updateHistory();
    } catch (error) {
      this.audioStatusText = 'AUDIO STATUS ERROR';
    }
  }

  private async startLiveAudio(): Promise<void> {
    if (this.engine === undefined) {
      this.audioStatusText = 'DSP OFFLINE';
      return;
    }

    this.audioStatusText = 'REQUESTING MICROPHONE';
    try {
      const permissions: Permissions[] = ['ohos.permission.MICROPHONE'];
      const manager = abilityAccessCtrl.createAtManager();
      const hostContext = this.getUIContext().getHostContext();
      if (hostContext === undefined) {
        this.audioStatusText = 'APPLICATION CONTEXT UNAVAILABLE';
        return;
      }
      const result = await manager.requestPermissionsFromUser(hostContext, permissions);
      if (result.authResults.length === 0 || result.authResults[0] !== 0) {
        this.audioStatusText = 'MICROPHONE PERMISSION REQUIRED';
        return;
      }

      this.applyAudioStatus(this.engine.startAudio());
    } catch (error) {
      this.audioStatusText = 'MICROPHONE REQUEST FAILED';
    }
  }

  private stopLiveAudio(): void {
    if (this.engine === undefined) {
      return;
    }
    this.applyAudioStatus(this.engine.stopAudio());
  }

  private runSelfTest(): void {
    if (this.engine === undefined) {
      this.engineState = 'DSP OFFLINE';
      return;
    }

    const frames = 4096;
    const input = new Float32Array(frames * 2);
    const output = new Float32Array(frames * 2);
    for (let frame = 0; frame < frames; frame++) {
      const sample = Math.sin(2 * Math.PI * 440 * frame / 48000) * 0.12;
      input[frame * 2] = sample;
      input[frame * 2 + 1] = sample * 0.94;
    }

    const meters: MeterSnapshot = this.engine.processInterleaved(input, output, frames);
    this.inputLufs = meters.inputLufs;
    this.outputLufs = meters.outputLufs;
    this.outputPeak = meters.outputPeak;
    this.selfTestPassed = output.some((sample: number) => Math.abs(sample) > 0.000001);
    this.engineState = this.selfTestPassed ? 'DSP VERIFIED' : 'CHECK REQUIRED';
  }

  @Builder
  private presets() {
    Column({ space: 8 }) {
      Text('PRESETS')
        .width('100%')
        .fontSize(12)
        .fontWeight(FontWeight.Bold)
        .fontColor('#AAB2BA')

      ForEach(PRESETS, (preset: Preset, index: number) => {
        Button() {
          Row() {
            Column({ space: 2 }) {
              Text(preset.name)
                .fontSize(12)
                .fontWeight(FontWeight.Medium)
                .fontColor(this.selectedPreset === index ? '#07100E' : '#E9ECEF')
              Text(preset.subtitle)
                .fontSize(11)
                .fontColor(this.selectedPreset === index ? '#25443F' : '#89939D')
            }
            .alignItems(HorizontalAlign.Start)
            Blank()
            if (this.selectedPreset === index) {
              Text('ACTIVE')
                .fontSize(10)
                .fontWeight(FontWeight.Bold)
                .fontColor('#173B35')
            }
          }
          .width('100%')
        }
        .width('100%')
        .height(48)
        .padding({ left: 12, right: 12 })
        .borderRadius(6)
        .backgroundColor(this.selectedPreset === index ? '#74B7AE' : '#22262A')
        .border({ width: 1, color: this.selectedPreset === index ? '#9BE1D7' : '#343A40' })
        .stateEffect(true)
        .onClick(() => {
          this.applyPreset(index);
        })
      }, (preset: Preset) => preset.name)
    }
    .width('100%')
  }

  @Builder
  private expertControls() {
    Column({ space: 14 }) {
      Row() {
        Text('PROCESS CHAIN')
          .fontSize(12)
          .fontWeight(FontWeight.Bold)
          .fontColor('#AAB2BA')
        Blank()
        Text('LIVE PARAMETERS')
          .fontSize(10)
          .fontColor('#6E7982')
      }
      .width('100%')
      .padding({ top: 4 })

      Row({ space: 18 }) {
        Column({ space: 10 }) {
          ParameterSlider({
            label: 'Gate threshold',
            valueText: `${this.gateThreshold.toFixed(1)} dB`,
            min: -90,
            max: 0,
            value: this.gateThreshold,
            onValueChange: (value: number) => {
              this.gateThreshold = value;
              this.setParameter(PARAM.GATE_THRESHOLD, value);
            }
          })
          ParameterSlider({
            label: 'EQ tilt',
            valueText: `${this.eqTilt.toFixed(1)} dB`,
            min: -6,
            max: 6,
            value: this.eqTilt,
            onValueChange: (value: number) => {
              this.eqTilt = value;
              this.setParameter(PARAM.EQ_TILT, value);
            }
          })
          ParameterSlider({
            label: 'Leveler speed',
            valueText: `${this.levelerSpeed.toFixed(0)}%`,
            min: 0,
            max: 100,
            value: this.levelerSpeed,
            onValueChange: (value: number) => {
              this.levelerSpeed = value;
              this.setParameter(PARAM.LEVELER_SPEED, value);
            }
          })
        }
        .layoutWeight(1)

        Column({ space: 10 }) {
          ParameterSlider({
            label: 'Knee compression',
            valueText: `${this.compressorStrength.toFixed(0)}%`,
            min: 0,
            max: 100,
            value: this.compressorStrength,
            onValueChange: (value: number) => {
              this.compressorStrength = value;
              this.setParameter(PARAM.COMP_STRENGTH, value);
            }
          })
          ParameterSlider({
            label: 'Limiter strength',
            valueText: `${this.limiterStrength.toFixed(0)}%`,
            min: 0,
            max: 100,
            value: this.limiterStrength,
            onValueChange: (value: number) => {
              this.limiterStrength = value;
              this.setParameter(PARAM.LIMITER_STRENGTH, value);
            }
          })
          ParameterSlider({
            label: 'Brickwall ceiling',
            valueText: `${this.ceiling.toFixed(1)} dB`,
            min: -3,
            max: 0,
            value: this.ceiling,
            onValueChange: (value: number) => {
              this.ceiling = value;
              this.setParameter(PARAM.BRICKWALL_CEILING, value);
            }
          })
        }
        .layoutWeight(1)
      }
      .width('100%')
    }
    .width('100%')
    .padding(16)
    .backgroundColor('#202428')
    .borderRadius(8)
    .border({ width: 1, color: '#343A40' })
  }

  build() {
    Column() {
      Row() {
        Row({ space: 12 }) {
          Stack() {
            Circle()
              .width(38)
              .height(38)
              .fill('#74B7AE')
            Text('m')
              .fontSize(24)
              .fontWeight(FontWeight.Bold)
              .fontColor('#13211E')
          }

          Column({ space: 1 }) {
            Text('master_me')
              .fontSize(28)
              .fontWeight(FontWeight.Medium)
              .fontColor('#F1F3F4')
            Text('AUTOMATIC MASTERING FOR HARMONYOS PC')
              .fontSize(10)
              .fontWeight(FontWeight.Medium)
              .fontColor('#7D8992')
              .letterSpacing(0.7)
          }
          .alignItems(HorizontalAlign.Start)
        }

        Blank()

        Row({ space: 10 }) {
          Circle()
            .width(7)
            .height(7)
            .fill(this.selfTestPassed ? '#74B7AE' : '#A15FC2')
          Text(this.engineState)
            .fontSize(10)
            .fontColor('#AAB2BA')
            .fontWeight(FontWeight.Medium)
        }

        Row({ space: 8 }) {
          Text(this.processorEnabled ? 'ENABLED' : 'BYPASSED')
            .fontSize(11)
            .fontWeight(FontWeight.Bold)
            .fontColor(this.processorEnabled ? '#DDF6F1' : '#89939D')
          Toggle({ type: ToggleType.Switch, isOn: this.processorEnabled })
            .selectedColor('#74B7AE')
            .onChange((value: boolean) => {
              this.processorEnabled = value;
              this.setParameter(PARAM.BYPASS, value ? 0 : 1);
            })
        }
        .padding({ left: 14, right: 14, top: 8, bottom: 8 })
        .backgroundColor('#202428')
        .borderRadius(8)
        .border({ width: 1, color: '#343A40' })
      }
      .width('100%')
      .height(78)
      .padding({ left: 26, right: 26 })
      .backgroundColor('#191C1F')
      .border({ width: { bottom: 1 }, color: '#343A40' })

      Row({ space: 18 }) {
        Column({ space: 14 }) {
          Row() {
            Text('INPUT')
              .fontSize(11)
              .fontWeight(FontWeight.Bold)
              .fontColor('#AAB2BA')
            Blank()
            Text(`${this.inputLufs.toFixed(1)} LUFS`)
              .fontSize(12)
              .fontColor('#E9ECEF')
          }
          .width('100%')

          Row({ space: 14 }) {
            LevelBar({ value: this.inputLufs, accent: '#74B7AE' })
            LevelBar({ value: this.inputLufs - 1.8, accent: '#74B7AE' })
            Column({ space: 4 }) {
              Text('TARGET')
                .fontSize(10)
                .fontColor('#7D8992')
              Text(this.targetLufs.toFixed(0))
                .fontSize(42)
                .fontWeight(FontWeight.Lighter)
                .fontColor('#F1F3F4')
              Text('LUFS')
                .fontSize(10)
                .fontColor('#74B7AE')
            }
            .alignItems(HorizontalAlign.Start)
          }
          .height(280)
          .width('100%')

          Column({ space: 9 }) {
            ParameterSlider({
              label: 'Input gain',
              valueText: `${this.inputGain.toFixed(1)} dB`,
              min: -24,
              max: 24,
              value: this.inputGain,
              onValueChange: (value: number) => {
                this.inputGain = value;
                this.setParameter(PARAM.INPUT_GAIN, value);
              }
            })
            ParameterSlider({
              label: 'Target',
              valueText: `${this.targetLufs.toFixed(0)} LUFS`,
              min: -30,
              max: -8,
              value: this.targetLufs,
              onValueChange: (value: number) => {
                this.targetLufs = value;
                this.setParameter(PARAM.TARGET, value);
              }
            })
          }
          .width('100%')

          Blank()

          Column({ space: 8 }) {
            Row({ space: 8 }) {
              Button('START LIVE')
                .layoutWeight(1)
                .height(36)
                .fontSize(10)
                .fontWeight(FontWeight.Bold)
                .fontColor('#12201D')
                .backgroundColor('#74B7AE')
                .borderRadius(6)
                .enabled(!this.audioRunning)
                .opacity(this.audioRunning ? 0.45 : 1)
                .onClick(() => {
                  this.startLiveAudio();
                })
              Button('STOP LIVE')
                .layoutWeight(1)
                .height(36)
                .fontSize(10)
                .fontWeight(FontWeight.Bold)
                .fontColor('#E2E6E9')
                .backgroundColor('#2B3035')
                .borderRadius(6)
                .enabled(this.audioRunning || this.audioRequested)
                .opacity(this.audioRunning || this.audioRequested ? 1 : 0.45)
                .onClick(() => {
                  this.stopLiveAudio();
                })
            }
            .width('100%')

            Row({ space: 6 }) {
              Circle()
                .width(6)
                .height(6)
                .fill(this.audioRunning ? '#74B7AE' : '#A15FC2')
              Text(this.audioStatusText)
                .fontSize(9)
                .fontColor(this.audioRunning ? '#A7DCD4' : '#8C969F')
                .maxLines(2)
            }
            .width('100%')

            Button() {
              Text(this.selfTestPassed ? 'PROCESSOR VERIFIED' : 'RUN ENGINE CHECK')
                .fontSize(10)
                .fontWeight(FontWeight.Bold)
                .fontColor(this.selfTestPassed ? '#173B35' : '#DDE1E5')
            }
            .width('100%')
            .height(34)
            .borderRadius(6)
            .backgroundColor(this.selfTestPassed ? '#74B7AE' : '#252A2F')
            .border({ width: 1, color: this.selfTestPassed ? '#9BE1D7' : '#3B4249' })
            .onClick(() => {
              this.runSelfTest();
            })
          }
          .width('100%')
        }
        .width(270)
        .height('100%')
        .padding(18)
        .backgroundColor('#1E2226')
        .borderRadius(8)
        .border({ width: 1, color: '#343A40' })

        Column({ space: 14 }) {
          Row({ space: 8 }) {
            Button('EASY')
              .height(34)
              .fontSize(11)
              .fontWeight(FontWeight.Bold)
              .fontColor(this.expertMode ? '#AAB2BA' : '#12201D')
              .backgroundColor(this.expertMode ? '#252A2F' : '#74B7AE')
              .borderRadius(5)
              .onClick(() => {
                this.expertMode = false;
              })
            Button('EXPERT')
              .height(34)
              .fontSize(11)
              .fontWeight(FontWeight.Bold)
              .fontColor(this.expertMode ? '#12201D' : '#AAB2BA')
              .backgroundColor(this.expertMode ? '#74B7AE' : '#252A2F')
              .borderRadius(5)
              .onClick(() => {
                this.expertMode = true;
              })
            Blank()
            Row({ space: 15 }) {
              Column({ space: 1 }) {
                Text('LUFS IN')
                  .fontSize(9)
                  .fontColor('#6E7982')
                Text(this.inputLufs.toFixed(1))
                  .fontSize(14)
                  .fontColor('#BFD8D4')
              }
              Column({ space: 1 }) {
                Text('LUFS OUT')
                  .fontSize(9)
                  .fontColor('#6E7982')
                Text(this.outputLufs.toFixed(1))
                  .fontSize(14)
                  .fontColor('#D8AFEA')
              }
              Column({ space: 1 }) {
                Text('PEAK OUT')
                  .fontSize(9)
                  .fontColor('#6E7982')
                Text(`${this.outputPeak.toFixed(1)} dB`)
                  .fontSize(14)
                  .fontColor('#E9ECEF')
              }
            }
          }
          .width('100%')

          Stack({ alignContent: Alignment.TopStart }) {
            Column() {
              Row() {
                Text('LOUDNESS HISTORY')
                  .fontSize(11)
                  .fontWeight(FontWeight.Bold)
                  .fontColor('#AAB2BA')
                Blank()
                Row({ space: 12 }) {
                  Row({ space: 5 }) {
                    Circle().width(7).height(7).fill('#74B7AE')
                    Text('input').fontSize(10).fontColor('#8C969F')
                  }
                  Row({ space: 5 }) {
                    Circle().width(7).height(7).fill('#C279D9')
                    Text('output').fontSize(10).fontColor('#8C969F')
                  }
                }
              }
              .width('100%')

              Blank()

              Row() {
                Text('-60 dB')
                  .fontSize(9)
                  .fontColor('#626C74')
                Blank()
                Text(this.audioRunning ?
                  `${Math.floor(this.callbackFrames)} live frames / ${this.underruns} gaps` :
                  '48 kHz / adaptive callback')
                  .fontSize(9)
                  .fontColor('#626C74')
              }
              .width('100%')
            }
            .width('100%')
            .height('100%')
            .padding(18)

            Canvas(this.historyContext)
              .width('100%')
              .height('100%')
              .margin({ top: 50 })
              .onReady(() => {
                this.canvasReady = true;
                this.drawHistory();
              })
          }
          .layoutWeight(1)
          .width('100%')
          .backgroundColor('#181B1E')
          .borderRadius(8)
          .border({ width: 1, color: '#343A40' })
          .clip(true)

          if (this.expertMode) {
            this.expertControls()
          } else {
            Row({ space: 12 }) {
              Column({ space: 4 }) {
                Text('EASY MODE')
                  .fontSize(10)
                  .fontWeight(FontWeight.Bold)
                  .fontColor('#74B7AE')
                Text('Choose a delivery preset. master_me balances tone, dynamics and loudness automatically.')
                  .fontSize(13)
                  .fontColor('#B8BEC5')
                  .lineHeight(19)
              }
              .layoutWeight(1)
              .alignItems(HorizontalAlign.Start)

              Column({ space: 3 }) {
                Text('CURRENT TARGET')
                  .fontSize(9)
                  .fontColor('#6E7982')
                Text(`${this.targetLufs.toFixed(0)} LUFS`)
                  .fontSize(22)
                  .fontWeight(FontWeight.Lighter)
                  .fontColor('#E8F6F3')
              }
              .alignItems(HorizontalAlign.End)
            }
            .width('100%')
            .padding(16)
            .backgroundColor('#202428')
            .borderRadius(8)
            .border({ width: 1, color: '#343A40' })
          }
        }
        .layoutWeight(1)
        .height('100%')

        Column({ space: 14 }) {
          Row() {
            Text('OUTPUT')
              .fontSize(11)
              .fontWeight(FontWeight.Bold)
              .fontColor('#AAB2BA')
            Blank()
            Text(`${this.outputLufs.toFixed(1)} LUFS`)
              .fontSize(12)
              .fontColor('#E9ECEF')
          }
          .width('100%')

          Row({ space: 14 }) {
            LevelBar({ value: this.outputLufs, accent: '#C279D9' })
            LevelBar({ value: this.outputLufs - 1.2, accent: '#C279D9' })
            Column({ space: 4 }) {
              Text('LIMIT')
                .fontSize(10)
                .fontColor('#7D8992')
              Text(this.ceiling.toFixed(1))
                .fontSize(42)
                  .fontWeight(FontWeight.Lighter)
                .fontColor('#F1F3F4')
              Text('CEILING dB')
                .fontSize(10)
                .fontColor('#C279D9')
            }
            .alignItems(HorizontalAlign.Start)
          }
          .height(280)
          .width('100%')

          Scroll() {
            this.presets()
          }
          .layoutWeight(1)
          .width('100%')
          .scrollBar(BarState.Off)
          .edgeEffect(EdgeEffect.Spring)

          Row() {
            Text('GPL-3.0')
              .fontSize(9)
              .fontColor('#606A72')
            Blank()
            Text('Faust + DPF')
              .fontSize(9)
              .fontColor('#606A72')
          }
          .width('100%')
        }
        .width(300)
        .height('100%')
        .padding(18)
        .backgroundColor('#1E2226')
        .borderRadius(8)
        .border({ width: 1, color: '#343A40' })
      }
      .layoutWeight(1)
      .width('100%')
      .padding(18)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#17191C')
  }

  private drawHistory(): void {
    if (!this.canvasReady) {
      return;
    }

    const context = this.historyContext;
    const width = context.width;
    const height = context.height;
    if (width <= 0 || height <= 0) {
      return;
    }

    context.clearRect(0, 0, width, height);
    context.lineWidth = 1;
    context.strokeStyle = '#262B30';
    for (let line = 1; line < 4; line++) {
      const y = height * line / 4;
      context.beginPath();
      context.moveTo(0, y);
      context.lineTo(width, y);
      context.stroke();
    }

    this.drawHistoryCurve(context, this.inputHistory, '#74B7AE', width, height);
    this.drawHistoryCurve(context, this.outputHistory, '#C279D9', width, height);
  }

  private updateHistory(): void {
    this.inputHistory.push(Math.max(0, Math.min(1, (this.inputLufs + 60) / 60)));
    this.outputHistory.push(Math.max(0, Math.min(1, (this.outputLufs + 60) / 60)));
    if (this.inputHistory.length > 120) {
      this.inputHistory.splice(0, this.inputHistory.length - 120);
      this.outputHistory.splice(0, this.outputHistory.length - 120);
    }
    this.drawHistory();
  }

  private drawHistoryCurve(context: CanvasRenderingContext2D, history: number[], color: string,
    width: number, height: number): void {
    if (history.length < 2) {
      return;
    }

    const step = width / Math.max(1, history.length - 1);
    context.beginPath();
    context.lineWidth = 2;
    context.strokeStyle = color;
    for (let index = 0; index < history.length; index++) {
      const x = index * step;
      const y = height - 8 - history[index] * Math.max(1, height - 16);
      if (index === 0) {
        context.moveTo(x, y);
      } else {
        context.lineTo(x, y);
      }
    }
    context.stroke();
  }
}
