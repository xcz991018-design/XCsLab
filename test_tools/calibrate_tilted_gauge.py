"""
Industrial Gauge Calibration Tool (Supports Outer Bezel, Inner Dial, or Center+Radius without Bezel)
用法: python test_tools/calibrate_tilted_gauge.py <image_path> [camera_id]

模式说明:
- 模式 1（4 点透视标定）：点击 4 个点（外框或表盘内刻度环 4 个特征角点），用于大角度倾斜拉平。
- 模式 2（2 点表轴标定，无需外框）：点击 2 个点（第1击点表轴中心铆钉，第2击点刻度线边缘），专用于无外框特写画面！
"""

import cv2
import numpy as np
import json
import sys
import os

pts = []
img_display = None
img_orig = None

def mouse_callback(event, x, y, flags, param):
    global pts, img_display
    if event == cv2.EVENT_LBUTTONDOWN:
        pts.append((float(x), float(y)))
        cv2.circle(img_display, (x, y), 6, (0, 255, 255), -1)
        
        if len(pts) == 1:
            print(f"[Point 1] Center Axis Pin: ({x}, {y})")
        elif len(pts) == 2:
            r_est = np.hypot(pts[1][0] - pts[0][0], pts[1][1] - pts[0][1])
            cv2.circle(img_display, (int(pts[0][0]), int(pts[0][1])), int(r_est), (255, 250, 0), 2)
            print(f"[Point 2] Scale Rim Point: ({x}, {y}) | Estimated Radius: {r_est:.1f}px")
            print("  -> Press '2' for 2-Point No-Bezel Center Mode, or click 2 more points for 4-Point Perspective Mode.")
        elif len(pts) == 3:
            cv2.line(img_display, (int(pts[1][0]), int(pts[1][1])), (x, y), (0, 255, 0), 2)
            print(f"[Point 3] Corner 3: ({x}, {y})")
        elif len(pts) == 4:
            cv2.line(img_display, (int(pts[2][0]), int(pts[2][1])), (x, y), (0, 255, 0), 2)
            cv2.line(img_display, (int(pts[3][0]), int(pts[3][1])), (int(pts[0][0]), int(pts[0][1])), (0, 255, 0), 2)
            print("\n✅ All 4 points selected for 4-Point Mode! Press '4' or ENTER to finish and save.")

        cv2.imshow("Gauge Calibration Tool", img_display)

def main():
    global img_orig, img_display, pts
    if len(sys.argv) < 2:
        print("Usage: python test_tools/calibrate_tilted_gauge.py <image_path> [camera_id]")
        return

    img_path = sys.argv[1]
    camera_id = sys.argv[2] if len(sys.argv) > 2 else "CAM_192.168.1.101"
    registry_file = "config/cameras.json"

    img_orig = cv2.imread(img_path)
    if img_orig is None:
        print(f"Error: Could not load image from {img_path}")
        return

    img_display = img_orig.copy()
    cv2.namedWindow("Gauge Calibration Tool", cv2.WINDOW_NORMAL)
    cv2.setMouseCallback("Gauge Calibration Tool", mouse_callback)

    print("================================================-----------------")
    print(f"Loaded Image: {img_path} ({img_orig.shape[1]}x{img_orig.shape[0]})")
    print(f"Target Camera ID: {camera_id}")
    print("-----------------------------------------------------------------")
    print("Option A (无外框特写 2 点模式): 点击 2 次 (①表轴中心铆钉 -> ②刻度圈边缘)，然后按按键 '2'")
    print("Option B (倾斜矫正 4 点模式): 顺时针点击 4 个角点 (外框或内盘刻度4点)，按任意键")
    print("================================================-----------------")

    cv2.imshow("Gauge Calibration Tool", img_display)
    key = cv2.waitKey(0)
    cv2.destroyAllWindows()

    new_cam_data = {}

    if len(pts) == 2 or key == ord('2'):
        # 2-Point No-Bezel Mode
        cx, cy = pts[0][0], pts[0][1]
        radius = np.hypot(pts[1][0] - pts[0][0], pts[1][1] - pts[0][1])
        new_cam_data = {
            "camera_id": camera_id,
            "perspective_correction_enabled": False,
            "auto_detect_roi": False,
            "center_x": cx,
            "center_y": cy,
            "radius": radius,
            "center_search_tolerance": 35.0
        }
        print(f"\n✅ Mode: 2-Point No-Bezel Calibration -> Center: ({cx:.1f}, {cy:.1f}), Radius: {radius:.1f}px")

    elif len(pts) == 4:
        # 4-Point Perspective Mode
        new_cam_data = {
            "camera_id": camera_id,
            "perspective_correction_enabled": True,
            "pt_0_x": pts[0][0], "pt_0_y": pts[0][1],
            "pt_1_x": pts[1][0], "pt_1_y": pts[1][1],
            "pt_2_x": pts[2][0], "pt_2_y": pts[2][1],
            "pt_3_x": pts[3][0], "pt_3_y": pts[3][1],
            "auto_detect_roi": False,
            "center_x": 400.0, "center_y": 400.0, "radius": 380.0,
            "center_search_tolerance": 35.0
        }
        print(f"\n✅ Mode: 4-Point Perspective Calibration -> 4 Points Saved.")

    else:
        print("Invalid number of points clicked. Aborting calibration.")
        return

    # Update config/cameras.json
    if os.path.exists(registry_file):
        with open(registry_file, "r", encoding="utf-8") as f:
            registry = json.load(f)
    else:
        registry = {"use_model": "gauge_profile.json", "cameras": []}

    cameras_list = registry.get("cameras", [])
    found_idx = -1
    for idx, cam in enumerate(cameras_list):
        if cam.get("camera_id") == camera_id:
            found_idx = idx
            break

    if found_idx >= 0:
        # Preserve non-calibrated fields like pointerBaseRadius, substation, etc.
        existing = cameras_list[found_idx]
        for k, v in existing.items():
            if k not in new_cam_data:
                new_cam_data[k] = v
        if "pointerBaseRadius" not in new_cam_data:
            new_cam_data["pointerBaseRadius"] = 55.0
        cameras_list[found_idx] = new_cam_data
        print(f"Updated existing camera [{camera_id}] in {registry_file}")
    else:
        new_cam_data["pointerBaseRadius"] = 55.0
        cameras_list.append(new_cam_data)
        print(f"Appended new camera [{camera_id}] to {registry_file}")

    registry["cameras"] = cameras_list
    with open(registry_file, "w", encoding="utf-8") as f:
        json.dump(registry, f, indent=2)

    print(f"Calibration saved successfully into: {registry_file}\n")

if __name__ == "__main__":
    main()
