package com.example.nfcapp;

import android.util.Log;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Pattern;

public class ASCIIDataParser {
    private static final String TAG = "ASCIIDataParser";
    
    public static class ASCIIDataInfo {
        public String rawHex;
        public String asciiData;
        public String deviceId;
        public String dutyCycle;
        public String temperature;
        public String humidity;
        public String otherInfo;
        public byte[] crcData;
        
        public ASCIIDataInfo(String rawHex, String asciiData, String deviceId, 
                           String dutyCycle, String temperature, String humidity, 
                           String otherInfo, byte[] crcData) {
            this.rawHex = rawHex;
            this.asciiData = asciiData;
            this.deviceId = deviceId;
            this.dutyCycle = dutyCycle;
            this.temperature = temperature;
            this.humidity = humidity;
            this.otherInfo = otherInfo;
            this.crcData = crcData;
        }
        
        @Override
        public String toString() {
            StringBuilder sb = new StringBuilder();
            sb.append("=== ASCII数据解析结果 ===\n");
            sb.append("原始HEX: ").append(rawHex).append("\n");
            sb.append("ASCII数据: ").append(asciiData).append("\n");
            if (deviceId != null && !deviceId.isEmpty()) {
                sb.append("设备ID: ").append(deviceId).append("\n");
            }
            if (dutyCycle != null && !dutyCycle.isEmpty()) {
                sb.append("占空比: ").append(dutyCycle).append("%\n");
            }
            if (temperature != null && !temperature.isEmpty()) {
                sb.append("温度: ").append(temperature).append("°C\n");
            }
            if (humidity != null && !humidity.isEmpty()) {
                sb.append("湿度: ").append(humidity).append("%\n");
            }
            if (otherInfo != null && !otherInfo.isEmpty()) {
                sb.append("其他信息: ").append(otherInfo).append("\n");
            }
            if (crcData != null && crcData.length > 0) {
                sb.append("CRC校验: ").append(byteArrayToHex(crcData)).append("\n");
            }
            return sb.toString();
        }
    }
    
    // 解析16进制字符串中的ASCII数据
    public static ASCIIDataInfo parseASCIIFromHex(String hexString) {
        try {
            // 清理16进制字符串
            hexString = hexString.replaceAll("\\s+", "").replaceAll("[^0-9A-Fa-f]", "");
            
            if (hexString.length() < 10) {
                return new ASCIIDataInfo(hexString, "", "", "", "", "", "", null);
            }
            
            // 转换为字节数组
            byte[] data = hexStringToByteArray(hexString);
            
            return parseASCIIFromBytes(data, hexString);
            
        } catch (Exception e) {
            Log.e(TAG, "Error parsing ASCII from hex", e);
            return new ASCIIDataInfo(hexString, "", "", "", "", "", "", null);
        }
    }
    
    // 从字节数组解析ASCII数据
    public static ASCIIDataInfo parseASCIIFromBytes(byte[] data, String originalHex) {
        String asciiData = "";
        String deviceId = "";
        String dutyCycle = "";
        String temperature = "";
        String humidity = "";
        String otherInfo = "";
        byte[] crcData = null;
        
        try {
            // 查找ASCII数据段
            List<String> asciiSegments = findASCIISegments(data);
            
            if (!asciiSegments.isEmpty()) {
                asciiData = String.join(" ", asciiSegments);
                
                // 解析每个ASCII段
                for (String segment : asciiSegments) {
                    // 查找占空比 (数字 + %)
                    if (segment.matches(".*\\d+.*")) {
                        // 提取数字
                        String[] numbers = segment.split("[^0-9]+");
                        for (String num : numbers) {
                            if (!num.isEmpty()) {
                                int value = Integer.parseInt(num);
                                if (value >= 0 && value <= 100) {
                                    dutyCycle = num;
                                    break;
                                }
                            }
                        }
                    }
                    
                    // 查找设备ID (字母数字组合)
                    if (segment.matches(".*[a-zA-Z]+.*")) {
                        if (deviceId.isEmpty()) {
                            deviceId = segment;
                        }
                    }
                    
                    // 查找温度 (包含温度相关字符)
                    if (segment.toLowerCase().contains("temp") || 
                        segment.toLowerCase().contains("t") ||
                        segment.matches(".*\\d+.*")) {
                        temperature = extractTemperature(segment);
                    }
                    
                    // 查找湿度 (包含湿度相关字符)
                    if (segment.toLowerCase().contains("hum") || 
                        segment.toLowerCase().contains("rh") ||
                        segment.matches(".*\\d+.*")) {
                        humidity = extractHumidity(segment);
                    }
                }
                
                // 如果没有找到具体分类，将所有ASCII数据作为其他信息
                if (deviceId.isEmpty() && dutyCycle.isEmpty() && 
                    temperature.isEmpty() && humidity.isEmpty()) {
                    otherInfo = asciiData;
                }
            }
            
            // 查找CRC数据 (通常在ASCII数据后面)
            crcData = findCRCData(data);
            
        } catch (Exception e) {
            Log.e(TAG, "Error parsing ASCII from bytes", e);
        }
        
        return new ASCIIDataInfo(originalHex, asciiData, deviceId, dutyCycle, 
                               temperature, humidity, otherInfo, crcData);
    }
    
    // 查找ASCII数据段
    private static List<String> findASCIISegments(byte[] data) {
        List<String> segments = new ArrayList<>();
        
        for (int i = 0; i < data.length - 1; i++) {
            // 查找连续的ASCII字符
            if (isASCIIPrintable(data[i])) {
                StringBuilder asciiBuilder = new StringBuilder();
                int j = i;
                
                // 收集连续的ASCII字符
                while (j < data.length && isASCIIPrintable(data[j])) {
                    asciiBuilder.append((char) (data[j] & 0xFF));
                    j++;
                }
                
                String asciiString = asciiBuilder.toString().trim();
                if (asciiString.length() >= 2) { // 至少2个字符才认为是有效ASCII
                    segments.add(asciiString);
                }
                
                i = j - 1; // 跳过已处理的字符
            }
        }
        
        return segments;
    }
    
    // 检查是否为可打印ASCII字符
    private static boolean isASCIIPrintable(byte b) {
        int value = b & 0xFF;
        return value >= 32 && value <= 126; // 可打印ASCII范围
    }
    
    // 提取温度信息
    private static String extractTemperature(String segment) {
        // 查找温度相关的数字
        String[] parts = segment.split("[^0-9.-]+");
        for (String part : parts) {
            if (!part.isEmpty()) {
                try {
                    double temp = Double.parseDouble(part);
                    if (temp >= -50 && temp <= 150) { // 合理温度范围
                        return part;
                    }
                } catch (NumberFormatException e) {
                    // 忽略非数字部分
                }
            }
        }
        return "";
    }
    
    // 提取湿度信息
    private static String extractHumidity(String segment) {
        // 查找湿度相关的数字
        String[] parts = segment.split("[^0-9.]+");
        for (String part : parts) {
            if (!part.isEmpty()) {
                try {
                    double hum = Double.parseDouble(part);
                    if (hum >= 0 && hum <= 100) { // 合理湿度范围
                        return part;
                    }
                } catch (NumberFormatException e) {
                    // 忽略非数字部分
                }
            }
        }
        return "";
    }
    
    // 查找CRC数据
    private static byte[] findCRCData(byte[] data) {
        // CRC通常在数据末尾，查找最后8-16字节的非ASCII数据
        for (int i = data.length - 16; i < data.length - 4; i++) {
            if (i >= 0) {
                boolean hasNonASCII = false;
                for (int j = i; j < Math.min(i + 8, data.length); j++) {
                    if (!isASCIIPrintable(data[j])) {
                        hasNonASCII = true;
                        break;
                    }
                }
                
                if (hasNonASCII) {
                    int crcLength = Math.min(8, data.length - i);
                    byte[] crc = new byte[crcLength];
                    System.arraycopy(data, i, crc, 0, crcLength);
                    return crc;
                }
            }
        }
        return null;
    }
    
    // 16进制字符串转字节数组
    private static byte[] hexStringToByteArray(String hexString) {
        int len = hexString.length();
        byte[] data = new byte[len / 2];
        for (int i = 0; i < len; i += 2) {
            data[i / 2] = (byte) ((Character.digit(hexString.charAt(i), 16) << 4)
                                 + Character.digit(hexString.charAt(i+1), 16));
        }
        return data;
    }
    
    // 字节数组转16进制字符串
    private static String byteArrayToHex(byte[] bytes) {
        StringBuilder result = new StringBuilder();
        for (byte b : bytes) {
            result.append(String.format("%02X ", b));
        }
        return result.toString().trim();
    }
}
