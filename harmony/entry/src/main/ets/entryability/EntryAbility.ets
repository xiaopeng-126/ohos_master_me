import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';

export default class EntryAbility extends UIAbility {
  onCreate(_want: Want, _launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(0x4d4d, 'master_me', 'HarmonyOS PC engine created');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(0x4d4d, 'master_me', 'Failed to load UI: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(0x4d4d, 'master_me', 'master_me UI loaded');
    });
  }
}
