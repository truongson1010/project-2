PIR SENSOR
PIR (Passive Infrared Sensor) là cảm biến hồng ngoại thụ động.
Nó không phát tia mà chỉ nhận bức xạ hồng ngoại (nhiệt) từ cơ thể người/động vật.
Output:
-Mức HIGH (1) → có chuyển động.
-Mức LOW (0) → không có chuyển động.
*Lưu ý:
Chân OUT (tín hiệu) của PIR module thường là open collector / TTL output.
Khi không có chuyển động, PIR có thể xuất ra LOW nhưng đôi khi bị “trôi” → đọc sai trên ESP32.
Vì vậy ta cần dùng pull-down resistor hoặc cấu hình internal pull-down của ESP32 để giữ nó ở mức 0 khi không hoạt động.
-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
LDR SENSOR ( cảm biến ánh sán)
ESP32 có ADC 12-bit → giá trị đọc ra nằm trong 0 → 4095.
adc1_config_width(ADC_WIDTH_BIT_12); (cấu hình độ phân giải 12-bit)
Khi ánh sáng rất mạnh, điện áp từ LDR đưa vào ADC gần bằng Vref (≈3.3V) → ADC sẽ saturate → trả về 4095.

Công thức đổi giá trị đọc được từ ADC qua giá trị điện áp

Raw ------- 0 -> 4095(2^12)
Vadc -------- 3.3V -> 5V(Vref)

Vadc= (Raw/2^N-1)*Vref​
Trong đó:
Raw = giá trị đọc được từ ADC
N = số bit độ phân giải ADC (ESP32 12-bit → 4095 max).
Vref = điện áp tham chiếu của ADC (thường ~ 3.3V trên ESP32, nhưng thực tế có thể lệch, thường 1.1V nội chuẩn hoặc 3.3V tùy chế độ).
-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------



