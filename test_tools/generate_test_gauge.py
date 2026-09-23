import cv2
import numpy as np
import math
import sys
import os

def create_synthetic_gauge(value_ratio=0.35, output_path="synthetic_gauge.png", gauge_val_str="0.35 MPa"):
    # 800x800 canvas
    size = 800
    img = np.ones((size, size, 3), dtype=np.uint8) * 240
    
    center = (400, 400)
    radius = 300
    
    # Outer bezel
    cv2.circle(img, center, radius + 20, (180, 180, 180), -1)
    cv2.circle(img, center, radius + 15, (60, 60, 60), 4)
    cv2.circle(img, center, radius, (255, 255, 255), -1)
    cv2.circle(img, center, radius, (80, 80, 80), 3)
    
    # Scale parameters
    # Min angle: 135 deg (lower left), Max angle: 45 deg (lower right), clockwise span: 270 deg
    min_angle = 135.0
    max_angle = 45.0
    span = 270.0
    
    # Draw tick marks & numbers
    num_ticks = 11  # 0.0 to 1.0 step 0.1
    for i in range(num_ticks):
        t = i / (num_ticks - 1)
        angle_deg = (min_angle + t * span) % 360.0
        rad = math.radians(angle_deg)
        
        # Outer tick point
        p_outer = (
            int(center[0] + (radius - 15) * math.cos(rad)),
            int(center[1] + (radius - 15) * math.sin(rad))
        )
        # Inner tick point
        is_major = (i % 2 == 0)
        tick_len = 25 if is_major else 12
        p_inner = (
            int(center[0] + (radius - 15 - tick_len) * math.cos(rad)),
            int(center[1] + (radius - 15 - tick_len) * math.sin(rad))
        )
        
        thickness = 3 if is_major else 1
        cv2.line(img, p_outer, p_inner, (30, 30, 30), thickness, cv2.LINE_AA)
        
        if is_major:
            val_num = round(t * 1.0, 1)
            p_text = (
                int(center[0] + (radius - 60) * math.cos(rad)) - 15,
                int(center[1] + (radius - 60) * math.sin(rad)) + 8
            )
            cv2.putText(img, f"{val_num}", p_text, cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 0), 2, cv2.LINE_AA)
            
    # Text logo
    cv2.putText(img, "PRESSURE GAUGE", (300, 310), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (50, 50, 50), 2, cv2.LINE_AA)
    cv2.putText(img, "MPa", (375, 490), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 0), 2, cv2.LINE_AA)

    # Pointer line
    ptr_angle_deg = (min_angle + value_ratio * span) % 360.0
    ptr_rad = math.radians(ptr_angle_deg)
    
    ptr_tip = (
        int(center[0] + (radius - 40) * math.cos(rad_ptr := ptr_rad)),
        int(center[1] + (radius - 40) * math.sin(rad_ptr))
    )
    
    # Draw Pointer
    cv2.line(img, center, ptr_tip, (20, 20, 20), 6, cv2.LINE_AA)
    cv2.circle(img, center, 25, (40, 40, 40), -1)
    cv2.circle(img, center, 10, (200, 200, 200), -1)
    
    cv2.imwrite(output_path, img)
    print(f"Generated synthetic gauge image: {output_path} (Target Value Ratio: {value_ratio}, Target Angle: {ptr_angle_deg:.1f} deg)")
    return output_path

if __name__ == "__main__":
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out_dir, exist_ok=True)
    
    # Generate 3 test samples: 0.18, 0.50, 0.82
    create_synthetic_gauge(0.18, os.path.join(out_dir, "test_gauge_0.18.png"))
    create_synthetic_gauge(0.50, os.path.join(out_dir, "test_gauge_0.50.png"))
    create_synthetic_gauge(0.82, os.path.join(out_dir, "test_gauge_0.82.png"))
