#include <cassert>
#include <iostream>
#include "common/SharedDetections.hpp"
int main() {
    // 同类别包含框在跨帧尺寸变化时必须复用同一条轨迹，不能在 display_hold
    // 期间让旧大框和新小框同时出现在发布快照中。
    TrackerConfig tracker_cfg;
    tracker_cfg.confirm_hits = 1;
    tracker_cfg.display_hold_ms = 100;
    tracker_cfg.align_to_video_pts = true;
    SharedDetections shared(tracker_cfg);
    Detection phone_outer{1243.5f, 347.28f, 1697.78f, 1065.84f, 0, "cell phone", 0.50f};
    Detection phone_inner{1301.03f, 932.0f, 1706.01f, 1071.29f, 0, "cell phone", 0.26f};
    shared.update({phone_outer}, 1, 1000, 3840, 2160);
    shared.update({phone_inner}, 2, 1033, 3840, 2160);
    assert(shared.get_for_pts(1033).detections.size() == 1);

    // 即使同一更新里已经存在两条重复轨迹，状态合并也要在发布前消除它们。
    SharedDetections duplicate_tracks(tracker_cfg);
    duplicate_tracks.update({phone_outer, phone_inner}, 1, 1500, 3840, 2160);
    assert(duplicate_tracks.get_for_pts(1500).detections.size() == 1);

    // 跨类别强包含候选先保持旧稳定框；连续两帧后才受控切换，期间始终只有一个框。
    SharedDetections switched_class(tracker_cfg);
    Detection keyboard{100, 100, 500, 500, 2, "keyboard", 0.70f};
    Detection laptop{150, 150, 450, 450, 4, "laptop", 0.75f};
    switched_class.update({keyboard}, 1, 2000, 3840, 2160);
    switched_class.update({laptop}, 2, 2033, 3840, 2160);
    auto pending_switch = switched_class.get_for_pts(2033).detections;
    assert(pending_switch.size() == 1 && pending_switch[0].class_id == 2);
    switched_class.update({laptop}, 3, 2066, 3840, 2160);
    auto confirmed_switch = switched_class.get_for_pts(2066).detections;
    assert(confirmed_switch.size() == 1 && confirmed_switch[0].class_id == 4);

    SharedDetections duplicate_classes(tracker_cfg);
    duplicate_classes.update({keyboard, laptop}, 1, 2500, 3840, 2160);
    assert(duplicate_classes.get_for_pts(2500).detections.size() == 1);

    std::cout << "image_processing_smoke: OK\n";
}
