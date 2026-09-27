package com.example.nfcapp;

import android.nfc.NdefMessage;
import android.nfc.NdefRecord;
import android.util.Log;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

public class NDEFParser {
    private static final String TAG = "NDEFParser";
    
    public static class NDEFRecordInfo {
        public String type;
        public String payload;
        public String description;
        public byte[] rawData;
        
        public NDEFRecordInfo(String type, String payload, String description, byte[] rawData) {
            this.type = type;
            this.payload = payload;
            this.description = description;
            this.rawData = rawData;
        }
        
        @Override
        public String toString() {
            return String.format("类型: %s\n内容: %s\n描述: %s", type, payload, description);
        }
    }
    
    public static class NDEFMessageInfo {
        public List<NDEFRecordInfo> records;
        public String summary;
        
        public NDEFMessageInfo(List<NDEFRecordInfo> records, String summary) {
            this.records = records;
            this.summary = summary;
        }
        
        @Override
        public String toString() {
            StringBuilder sb = new StringBuilder();
            sb.append("NDEF消息摘要: ").append(summary).append("\n");
            sb.append("记录数量: ").append(records.size()).append("\n\n");
            
            for (int i = 0; i < records.size(); i++) {
                sb.append("记录 ").append(i + 1).append(":\n");
                sb.append(records.get(i).toString()).append("\n\n");
            }
            
            return sb.toString();
        }
    }
    
    // 解析16进制字符串为NDEF数据
    public static NDEFMessageInfo parseHexToNDEF(String hexString) {
        try {
            // 移除空格和分隔符
            hexString = hexString.replaceAll("\\s+", "").replaceAll("[^0-9A-Fa-f]", "");
            
            // 转换为字节数组
            byte[] data = hexStringToByteArray(hexString);
            
            return parseNDEFData(data);
        } catch (Exception e) {
            Log.e(TAG, "Error parsing hex to NDEF", e);
            return new NDEFMessageInfo(new ArrayList<>(), "解析失败: " + e.getMessage());
        }
    }
    
    // 解析字节数组为NDEF数据
    public static NDEFMessageInfo parseNDEFData(byte[] data) {
        List<NDEFRecordInfo> records = new ArrayList<>();
        String summary = "";
        
        try {
            // 检查是否是有效的NDEF消息
            if (data.length < 3) {
                return new NDEFMessageInfo(records, "数据太短，不是有效的NDEF消息");
            }
            
            // 检查NDEF消息头
            int offset = 0;
            
            // 读取消息头
            byte flags = data[offset++];
            byte typeLength = data[offset++];
            int payloadLength = data[offset++] & 0xFF;
            
            // 检查是否是短记录格式
            boolean shortRecord = (flags & 0x10) != 0;
            if (!shortRecord && data.length > offset + 3) {
                payloadLength = ((data[offset++] & 0xFF) << 24) |
                              ((data[offset++] & 0xFF) << 16) |
                              ((data[offset++] & 0xFF) << 8) |
                              (data[offset++] & 0xFF);
            }
            
            // 读取ID长度（如果存在）
            int idLength = 0;
            if ((flags & 0x08) != 0) {
                idLength = data[offset++] & 0xFF;
            }
            
            // 读取类型
            byte[] type = new byte[typeLength];
            System.arraycopy(data, offset, type, 0, typeLength);
            offset += typeLength;
            
            // 读取ID（如果存在）
            if (idLength > 0) {
                offset += idLength;
            }
            
            // 读取载荷
            byte[] payload = new byte[payloadLength];
            System.arraycopy(data, offset, payload, 0, payloadLength);
            
            // 解析记录
            NDEFRecordInfo record = parseNDEFRecord(type, payload, data);
            records.add(record);
            
            summary = String.format("NDEF消息 - 类型: %s, 载荷长度: %d字节", 
                                  new String(type, StandardCharsets.UTF_8), payloadLength);
            
        } catch (Exception e) {
            Log.e(TAG, "Error parsing NDEF data", e);
            summary = "解析错误: " + e.getMessage();
        }
        
        return new NDEFMessageInfo(records, summary);
    }
    
    // 解析NDEF记录
    private static NDEFRecordInfo parseNDEFRecord(byte[] type, byte[] payload, byte[] rawData) {
        String typeStr = new String(type, StandardCharsets.UTF_8);
        String payloadStr = "";
        String description = "";
        
        try {
            // 根据类型解析载荷
            switch (typeStr) {
                case "T":
                    // 文本记录
                    if (payload.length > 0) {
                        int languageCodeLength = payload[0] & 0x3F;
                        if (payload.length > languageCodeLength + 1) {
                            payloadStr = new String(payload, languageCodeLength + 1, 
                                                  payload.length - languageCodeLength - 1, 
                                                  StandardCharsets.UTF_8);
                        }
                    }
                    description = "文本记录";
                    break;
                    
                case "U":
                    // URI记录
                    if (payload.length > 0) {
                        String uriPrefix = getURIPrefix(payload[0] & 0xFF);
                        if (payload.length > 1) {
                            payloadStr = uriPrefix + new String(payload, 1, payload.length - 1, 
                                                              StandardCharsets.UTF_8);
                        } else {
                            payloadStr = uriPrefix;
                        }
                    }
                    description = "URI记录";
                    break;
                    
                case "Sp":
                    // 智能海报
                    description = "智能海报记录";
                    payloadStr = "智能海报数据 (" + payload.length + " 字节)";
                    break;
                    
                default:
                    // 其他类型
                    description = "未知记录类型: " + typeStr;
                    payloadStr = "原始数据 (" + payload.length + " 字节)";
                    break;
            }
        } catch (Exception e) {
            Log.e(TAG, "Error parsing NDEF record", e);
            description = "解析错误: " + e.getMessage();
            payloadStr = "无法解析的载荷";
        }
        
        return new NDEFRecordInfo(typeStr, payloadStr, description, rawData);
    }
    
    // 获取URI前缀
    private static String getURIPrefix(int code) {
        switch (code) {
            case 0x01: return "http://www.";
            case 0x02: return "https://www.";
            case 0x03: return "http://";
            case 0x04: return "https://";
            case 0x05: return "tel:";
            case 0x06: return "mailto:";
            case 0x07: return "ftp://anonymous:anonymous@";
            case 0x08: return "ftp://ftp.";
            case 0x09: return "ftps://";
            case 0x0A: return "sftp://";
            case 0x0B: return "smb://";
            case 0x0C: return "nfs://";
            case 0x0D: return "ftp://";
            case 0x0E: return "dav://";
            case 0x0F: return "news:";
            case 0x10: return "telnet://";
            case 0x11: return "imap:";
            case 0x12: return "rtsp://";
            case 0x13: return "urn:";
            case 0x14: return "pop:";
            case 0x15: return "sip:";
            case 0x16: return "sips:";
            case 0x17: return "tftp:";
            case 0x18: return "btspp://";
            case 0x19: return "btl2cap://";
            case 0x1A: return "btgoep://";
            case 0x1B: return "tcpobex://";
            case 0x1C: return "irdaobex://";
            case 0x1D: return "file://";
            case 0x1E: return "urn:epc:id:";
            case 0x1F: return "urn:epc:tag:";
            case 0x20: return "urn:epc:pat:";
            case 0x21: return "urn:epc:raw:";
            case 0x22: return "urn:epc:";
            case 0x23: return "urn:nfc:";
            default: return "";
        }
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
    public static String byteArrayToHexString(byte[] bytes) {
        StringBuilder result = new StringBuilder();
        for (byte b : bytes) {
            result.append(String.format("%02X ", b));
        }
        return result.toString().trim();
    }
}
