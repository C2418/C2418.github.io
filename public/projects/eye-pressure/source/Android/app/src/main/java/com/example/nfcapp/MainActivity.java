package com.example.nfcapp;

import android.Manifest;
import android.app.AlertDialog;
import android.bluetooth.BluetoothDevice;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.text.method.ScrollingMovementMethod;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;
import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class MainActivity extends AppCompatActivity {
    private static final String TAG = "MainActivity";

    private TextView statusText;
    private TextView deviceInfoText;
    private TextView asciiInfoText;
    private LinearLayout connectionButtons;
    private LinearLayout controlButtons;
    private Button btnViewLog;
    private Button btnSetVoltage;
    private Button btnCloseVoltage;
    private Button btnReadDevice;
    private EditText voltageInput;
    private LinearLayout buttonGrid;
    private TextView dataLog;

    private NFCBluetoothManager bluetoothManager;
    private BluetoothDevice targetDevice;
    private boolean isConnected = false;
    private String lastAsciiDisplayText = "";

    // 性能优化字段
    private final ExecutorService parseExecutor = Executors.newSingleThreadExecutor();
    private static final int MAX_LOG_LINES = 500; // 限制日志最大行数
    private long lastUiUpdateMs = 0L; // 日志滚动与状态防抖
    private boolean testButtonsAdded = false; // 防止重复添加测试按钮

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        Log.d(TAG, "MainActivity onCreate called");
        
        try {
            initializeViews();
            bluetoothManager = new NFCBluetoothManager(this, this);

            setupBluetoothManagerListeners();
            checkPermissionsAndStart();
            
            Log.d(TAG, "MainActivity initialized successfully");
            
        } catch (Exception e) {
            Log.e(TAG, "Error in onCreate", e);
            Toast.makeText(this, "应用初始化失败: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private void initializeViews() {
        try {
            statusText = findViewById(R.id.statusText);
            deviceInfoText = findViewById(R.id.deviceInfoText);
            asciiInfoText = findViewById(R.id.asciiInfoText);
            connectionButtons = findViewById(R.id.connectionButtons);
            controlButtons = findViewById(R.id.controlButtons);
            btnViewLog = findViewById(R.id.btnViewLog);
            btnSetVoltage = findViewById(R.id.btnSetVoltage);
            btnCloseVoltage = findViewById(R.id.btnCloseVoltage);
            btnReadDevice = findViewById(R.id.btnReadDevice);
            voltageInput = findViewById(R.id.voltageInput);
            buttonGrid = findViewById(R.id.buttonGrid);
            dataLog = findViewById(R.id.dataLog);

            Button btnYes = findViewById(R.id.btnYes);
            Button btnNo = findViewById(R.id.btnNo);
            Button btnScan = findViewById(R.id.btnScan);
            Button btnConnect = findViewById(R.id.btnConnect);

            btnYes.setOnClickListener(v -> connectToDevice());
            btnNo.setOnClickListener(v -> declineConnection());
            btnScan.setOnClickListener(v -> startDeviceScan());
            btnConnect.setOnClickListener(v -> connectToDevice());

            btnViewLog.setOnClickListener(v -> {
                try {
                    android.app.AlertDialog.Builder builder = new android.app.AlertDialog.Builder(MainActivity.this);
                    builder.setTitle("日志");
                    final android.widget.ScrollView scrollView = new android.widget.ScrollView(MainActivity.this);
                    final android.widget.TextView logView = new android.widget.TextView(MainActivity.this);
                    logView.setText(dataLog.getText());
                    logView.setTextSize(12f);
                    logView.setPadding(24, 24, 24, 24);
                    logView.setScrollBarStyle(android.view.View.SCROLLBARS_INSIDE_INSET);
                    scrollView.addView(logView);
                    builder.setView(scrollView);
                    builder.setPositiveButton("关闭", (d, w) -> d.dismiss());
                    builder.show();
                } catch (Exception e) {
                    Log.e(TAG, "Error showing log dialog", e);
                }
            });

            btnSetVoltage.setOnClickListener(v -> {
                try {
                    String val = voltageInput.getText().toString().trim();
                    double voltage = Double.parseDouble(val);
                    if (voltage < 0.036 || voltage > 1.80) {
                        Toast.makeText(MainActivity.this, "电压范围应为0.036-1.80V", Toast.LENGTH_SHORT).show();
                        return;
                    }
                    int duty = calculateDutyFromVoltage(voltage);
                    sendDutyCycleCommand(duty);
                } catch (NumberFormatException e) {
                    Toast.makeText(MainActivity.this, "请输入有效数字", Toast.LENGTH_SHORT).show();
                } catch (Exception e) {
                    Log.e(TAG, "Error setting voltage", e);
                }
            });

            btnCloseVoltage.setOnClickListener(v -> {
                try {
                    String hexString = "20 00 53 05 1C 00 11 22 64 E2 03";
                    byte[] testData = parseHexString(hexString);
                    if (testData != null) {
                        bluetoothManager.sendData(testData);
                        logData("关闭电压输出: " + hexString);
                    } else {
                        logData("发送失败: 无效的16进制数据");
                    }
                } catch (Exception e) {
                    Log.e(TAG, "Error closing voltage", e);
                }
            });

            btnReadDevice.setOnClickListener(v -> {
                try {
                    String hexString = "20 00 57 02 00 2C 86 03 0A 0D";
                    byte[] testData = parseHexString(hexString);
                    if (testData != null) {
                        bluetoothManager.sendData(testData);
                        logData("读取设备信息: " + hexString);
                    } else {
                        logData("发送失败: 无效的16进制数据");
                    }
                } catch (Exception e) {
                    Log.e(TAG, "Error reading device", e);
                }
            });

            dataLog.setMovementMethod(new ScrollingMovementMethod());
            
            Log.d(TAG, "Views initialized successfully");
        } catch (Exception e) {
            Log.e(TAG, "Error initializing views", e);
            throw e;
        }
    }

    private void setupBluetoothManagerListeners() {
        try {
            bluetoothManager.setDeviceFoundListener(new NFCBluetoothManager.OnDeviceFoundListener() {
                @Override
                public void onDeviceFound(BluetoothDevice device) {
                    targetDevice = device;
                    runOnUiThread(() -> {
                        statusText.setText(R.string.device_found);
                        deviceInfoText.setText(String.format(getString(R.string.connect_to_device), device.getName()));
                        connectionButtons.setVisibility(View.VISIBLE);
                    });
                }

                @Override
                public void onNoDeviceFound() {
                    runOnUiThread(() -> {
                        statusText.setText(R.string.device_not_found);
                        deviceInfoText.setText("");
                        connectionButtons.setVisibility(View.GONE);
                        controlButtons.setVisibility(View.VISIBLE);
                    });
                }
            });

            bluetoothManager.setConnectionStateListener(new NFCBluetoothManager.OnConnectionStateChangeListener() {
                @Override
                public void onConnectionStateChanged(boolean connected) {
                    isConnected = connected;
                    runOnUiThread(() -> {
                        if (connected) {
                            // 顶部状态显示已隐藏，不再更新文案
                            connectionButtons.setVisibility(View.GONE);
                            controlButtons.setVisibility(View.GONE);
                            voltageInput.setVisibility(View.VISIBLE);
                            buttonGrid.setVisibility(View.VISIBLE);
                            dataLog.setVisibility(View.GONE);
                            
                            String timestamp = java.text.DateFormat.getTimeInstance().format(new java.util.Date());
                            logData(String.format("[%s] %s", timestamp, getString(R.string.connected)));
                            
                            // 打印服务信息用于调试
                            bluetoothManager.printServiceInfo();
                            
                            // 添加测试按钮
                            addTestButton();
                        } else {
                            // 断开连接时隐藏ASCII信息
                            asciiInfoText.setVisibility(View.GONE);
                            voltageInput.setVisibility(View.GONE);
                            buttonGrid.setVisibility(View.GONE);
                            dataLog.setVisibility(View.GONE);
                        }
                    });
                }

                @Override
                public void onConnectionFailed() {
                    runOnUiThread(() -> {
                        Toast.makeText(MainActivity.this, R.string.connection_failed, Toast.LENGTH_SHORT).show();
                        connectionButtons.setVisibility(View.GONE);
                        controlButtons.setVisibility(View.VISIBLE);
                    });
                }
            });

            bluetoothManager.setDataReceivedListener(data -> {
                Log.d(TAG, "Received raw data bytes: " + java.util.Arrays.toString(data));
                
                // 将字节数组转换为16进制字符串
                StringBuilder hexString = new StringBuilder();
                for (byte b : data) {
                    hexString.append(String.format("%02X ", b));
                }
                String hexData = hexString.toString().trim();
                Log.d(TAG, "Received hex data: " + hexData);

                // 先更新日志与状态（在UI线程，且做防抖）
                runOnUiThread(() -> {
                    String timestamp = java.text.DateFormat.getTimeInstance().format(new java.util.Date());
                    String logMessage = String.format("[%s] 接收(HEX): %s", timestamp, hexData);
                    logData(logMessage);

                    long now = System.currentTimeMillis();
                    if (now - lastUiUpdateMs >= 120) { // 120ms防抖
                        lastUiUpdateMs = now;
                    }
                });

                // 将ASCII解析放到后台线程，完成后回到UI线程更新显示
                parseExecutor.submit(() -> {
                    try {
                        ASCIIDataParser.ASCIIDataInfo asciiInfo = ASCIIDataParser.parseASCIIFromHex(hexData);
                        if (asciiInfo != null && asciiInfo.asciiData != null && !asciiInfo.asciiData.isEmpty()) {
                            runOnUiThread(() -> {
                                logData("ASCII数据解析:");
                                logData(asciiInfo.toString());
                                updateASCIIInfoDisplay(asciiInfo);
                            });
                        }
                    } catch (Exception e) {
                        Log.d(TAG, "ASCII parsing failed: " + e.getMessage());
                    }
                });
            });
            
            Log.d(TAG, "Bluetooth listeners set up successfully");
        } catch (Exception e) {
            Log.e(TAG, "Error setting up bluetooth listeners", e);
            throw e;
        }
    }

    private void checkPermissionsAndStart() {
        try {
            if (!bluetoothManager.isBluetoothSupported()) {
                Toast.makeText(this, R.string.bluetooth_not_supported, Toast.LENGTH_LONG).show();
                return;
            }

            if (!bluetoothManager.hasLocationPermission()) {
                bluetoothManager.requestLocationPermission();
                return;
            }

            if (!bluetoothManager.hasBluetoothPermissions()) {
                bluetoothManager.requestBluetoothPermissions();
                return;
            }

            if (!bluetoothManager.isBluetoothEnabled()) {
                bluetoothManager.enableBluetooth();
                return;
            }

            startDeviceScan();
            Log.d(TAG, "Permissions checked and device scan started");
        } catch (Exception e) {
            Log.e(TAG, "Error checking permissions", e);
            Toast.makeText(this, "权限检查失败: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private void startDeviceScan() {
        try {
            statusText.setText(R.string.scanning_devices);
            deviceInfoText.setText("");
            connectionButtons.setVisibility(View.GONE);
            controlButtons.setVisibility(View.GONE);
            bluetoothManager.startScanning();
            Log.d(TAG, "Device scan started");
        } catch (Exception e) {
            Log.e(TAG, "Error starting device scan", e);
            Toast.makeText(this, "设备扫描失败: " + e.getMessage(), Toast.LENGTH_SHORT).show();
        }
    }

    private void connectToDevice() {
        try {
            if (targetDevice != null) {
                statusText.setText(R.string.connecting);
                deviceInfoText.setText("");
                connectionButtons.setVisibility(View.GONE);
                bluetoothManager.connectToDevice(targetDevice);
                Log.d(TAG, "Connecting to device: " + targetDevice.getName());
            }
        } catch (Exception e) {
            Log.e(TAG, "Error connecting to device", e);
            Toast.makeText(this, "连接失败: " + e.getMessage(), Toast.LENGTH_SHORT).show();
        }
    }

    private void declineConnection() {
        targetDevice = null;
        startDeviceScan();
    }


    
    private void sendDutyCycleCommand(int dutyPercent) {
        try {
            // duty转为1字节16进制（0-100）
            int dc = dutyPercent & 0xFF;
            // 按协议构造：20 00 53 05 1C 00 11 22 <dc> <ecc> 03
            byte[] payload = new byte[] {
                (byte)0x20, (byte)0x00, (byte)0x53, (byte)0x05,
                (byte)0x1C, // 写入地址
                (byte)0x00, (byte)0x11, (byte)0x22, // 命令前缀
                (byte)dc,
                (byte)0x00, // 占位, 稍后填充ECC
                (byte)0x03
            };

            // 计算ECC: 对 00 53 05 1C 00 11 22 <dc> 逐字节异或后再取反
            int xor = 0x00;
            int[] eccRange = new int[] {0x00, 0x53, 0x05, 0x1C, 0x00, 0x11, 0x22, dc};
            for (int v : eccRange) {
                xor ^= v & 0xFF;
            }
            int ecc = (~xor) & 0xFF;
            payload[9] = (byte) ecc; // 写入ECC

            // 发送
            bluetoothManager.sendData(payload);

            // 记录日志（HEX）
            StringBuilder sb = new StringBuilder();
            for (byte b : payload) sb.append(String.format("%02X ", b));
            String hexOut = sb.toString().trim();
            String timestamp = java.text.DateFormat.getTimeInstance().format(new java.util.Date());
            logData(String.format("[%s] 发送(设置电压对应占空比 %d%%): %s", timestamp, dutyPercent, hexOut));
        } catch (Exception e) {
            Log.e(TAG, "Error sending duty command", e);
            Toast.makeText(this, "发送占空比命令失败: " + e.getMessage(), Toast.LENGTH_SHORT).show();
        }
    }

    // 解析16进制字符串为字节数组
    private byte[] parseHexString(String hexString) {
        try {
            // 移除空格和分隔符
            hexString = hexString.replaceAll("\\s+", "").replaceAll("[^0-9A-Fa-f]", "");
            
            // 确保是偶数长度
            if (hexString.length() % 2 != 0) {
                hexString = "0" + hexString;
            }
            
            byte[] result = new byte[hexString.length() / 2];
            for (int i = 0; i < result.length; i++) {
                int index = i * 2;
                result[i] = (byte) Integer.parseInt(hexString.substring(index, index + 2), 16);
            }
            return result;
        } catch (Exception e) {
            Log.e(TAG, "Error parsing hex string: " + hexString, e);
            return null;
        }
    }

    private void logData(String message) {
        try {
            String currentText = dataLog.getText().toString();
            String newText = currentText.isEmpty() ? message : currentText + "\n" + message;

            // 限制日志行数，保留最后 MAX_LOG_LINES 行
            int lines = 0;
            for (int i = newText.length() - 1; i >= 0; i--) {
                if (newText.charAt(i) == '\n') {
                    lines++;
                    if (lines >= MAX_LOG_LINES) {
                        // 从下数第 MAX_LOG_LINES 个换行符的下一位开始保留
                        newText = newText.substring(i + 1);
                        break;
                    }
                }
            }

            dataLog.setText(newText);

            // 滚动到底部（防止在未布局完成时NPE）
            if (dataLog.getLayout() != null) {
                int scrollAmount = dataLog.getLayout().getLineTop(dataLog.getLineCount()) - dataLog.getHeight();
                if (scrollAmount > 0) {
                    dataLog.scrollTo(0, scrollAmount);
                } else {
                    dataLog.scrollTo(0, 0);
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "Error logging data", e);
        }
    }
    
    // 更新ASCII信息显示
    private void updateASCIIInfoDisplay(ASCIIDataParser.ASCIIDataInfo asciiInfo) {
        try {
            StringBuilder displayText = new StringBuilder();

            // 检查原始ASCII数据是否符合 "\H enxx xx%" 格式
            boolean isSpecialFormat = false;
            String lang = "";
            String number = "";
            String dutyValue = "";
            
            if (asciiInfo.asciiData != null && !asciiInfo.asciiData.isEmpty()) {
                // 匹配 \H enxx xx% 格式 (其中\H可能是特殊字符)
                String pattern = ".*\\s*([a-zA-Z]{2})(\\d+)\\s+(\\d+)%?.*";
                java.util.regex.Pattern regex = java.util.regex.Pattern.compile(pattern);
                java.util.regex.Matcher matcher = regex.matcher(asciiInfo.asciiData);
                
                if (matcher.find()) {
                    lang = matcher.group(1);
                    number = matcher.group(2);
                    dutyValue = matcher.group(3);
                    isSpecialFormat = true;
                }
            }

            // 如果不是特殊格式，检查标准格式：设备ID为两位字母+数字，占空比为0-100
            boolean hasStdDeviceId = false;
            boolean hasStdDuty = false;
            
            if (!isSpecialFormat) {
                if (asciiInfo.deviceId != null) {
                    String id = asciiInfo.deviceId.trim();
                    if (id.matches("^[A-Za-z]{2}\\d+.*")) {
                        lang = id.substring(0, 2);
                        number = id.substring(2).replaceAll("[^0-9]", "");
                        hasStdDeviceId = !number.isEmpty();
                    }
                }
                if (asciiInfo.dutyCycle != null && !asciiInfo.dutyCycle.isEmpty()) {
                    try {
                        int duty = Integer.parseInt(asciiInfo.dutyCycle);
                        hasStdDuty = duty >= 0 && duty <= 100;
                        dutyValue = asciiInfo.dutyCycle;
                    } catch (NumberFormatException ignored) {}
                }
            }

            boolean isStandard = isSpecialFormat || (hasStdDeviceId && hasStdDuty);

            if (isStandard) {
                displayText.append("ASCII信息显示区域\n");
                displayText.append("电阻: ");
                displayText.append("（").append(lang).append("）").append(number).append("\n");
                
                // 计算对应的电压值
                try {
                    int duty = Integer.parseInt(dutyValue);
                    double voltage = calculateVoltageFromDuty(duty);
                    displayText.append("电压: ").append(String.format("%.3f", voltage)).append("V\n");
                } catch (NumberFormatException e) {
                    // 如果占空比解析失败，不显示电压
                }

                String finalText = displayText.toString().trim();
                asciiInfoText.setText(finalText);
                asciiInfoText.setVisibility(View.VISIBLE);
                lastAsciiDisplayText = finalText; // 缓存为上次测量值
            } else {
                // 非标准：显示上次测量值（若存在），否则不更新
                if (lastAsciiDisplayText != null && !lastAsciiDisplayText.isEmpty()) {
                    asciiInfoText.setText(lastAsciiDisplayText);
                    asciiInfoText.setVisibility(View.VISIBLE);
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "Error updating ASCII info display", e);
        }
    }
    
    // 根据占空比计算对应电压值（线性插值）
    private double calculateVoltageFromDuty(int dutyCycle) {
        // 数据点：占空比 -> 电压
        int[] dutyPoints = {1, 10, 20, 30, 40, 50, 60, 70, 80, 90, 99};
        double[] voltagePoints = {1.80, 1.64, 1.46, 1.30, 1.1, 0.92, 0.74, 0.569, 0.378, 0.202, 0.036};
        
        // 边界处理
        if (dutyCycle <= 1) return 1.80;
        if (dutyCycle >= 99) return 0.036;
        
        // 线性插值
        for (int i = 0; i < dutyPoints.length - 1; i++) {
            if (dutyCycle >= dutyPoints[i] && dutyCycle <= dutyPoints[i + 1]) {
                double x1 = dutyPoints[i];
                double y1 = voltagePoints[i];
                double x2 = dutyPoints[i + 1];
                double y2 = voltagePoints[i + 1];
                
                // 线性插值公式: y = y1 + (y2-y1) * (x-x1) / (x2-x1)
                return y1 + (y2 - y1) * (dutyCycle - x1) / (x2 - x1);
            }
        }
        
        return 0.0; // 默认值
    }
    
    // 根据电压计算对应占空比（反向线性插值）
    private int calculateDutyFromVoltage(double voltage) {
        // 数据点：占空比 -> 电压
        int[] dutyPoints = {1, 10, 20, 30, 40, 50, 60, 70, 80, 90, 99};
        double[] voltagePoints = {1.80, 1.64, 1.46, 1.30, 1.1, 0.92, 0.74, 0.569, 0.378, 0.202, 0.036};
        
        // 边界处理
        if (voltage >= 1.80) return 1;
        if (voltage <= 0.036) return 99;
        
        // 反向线性插值
        for (int i = 0; i < voltagePoints.length - 1; i++) {
            if (voltage <= voltagePoints[i] && voltage >= voltagePoints[i + 1]) {
                double y1 = voltagePoints[i];
                double x1 = dutyPoints[i];
                double y2 = voltagePoints[i + 1];
                double x2 = dutyPoints[i + 1];
                
                // 反向线性插值公式: x = x1 + (x2-x1) * (y-y1) / (y2-y1)
                return (int) Math.round(x1 + (x2 - x1) * (voltage - y1) / (y2 - y1));
            }
        }
        
        return 50; // 默认值
    }
    
    // 添加测试按钮用于调试
    private void addTestButton() {
        try {
            if (testButtonsAdded) return; // 已添加过则跳过
            testButtonsAdded = true;
            // 发送测试按钮
            Button testButton = new Button(this);
            testButton.setText("关闭电压输出");
            testButton.setOnClickListener(v -> {
                // 解析16进制字符串为字节数组
                String hexString = "20 00 53 05 1C 00 11 22 64 E2 03";
                byte[] testData = parseHexString(hexString);
                if (testData != null) {
                    bluetoothManager.sendData(testData);
                    logData("关闭电压输出: " + hexString);
                } else {
                    logData("发送失败: 无效的16进制数据");
                }
            });
            
            // 新的测试发送按钮
            Button newTestButton = new Button(this);
            newTestButton.setText("读取设备信息");
            newTestButton.setOnClickListener(v -> {
                // 解析16进制字符串为字节数组
                String hexString = "20 00 57 02 00 2C 86 03 0A 0D";
                byte[] testData = parseHexString(hexString);
                if (testData != null) {
                    bluetoothManager.sendData(testData);
                    logData("读取设备信息: " + hexString);
                } else {
                    logData("发送失败: 无效的16进制数据");
                }
            });
            
            // 将测试按钮添加到布局中
            LinearLayout layout = findViewById(R.id.dataLog).getParent() instanceof LinearLayout ? 
                (LinearLayout) findViewById(R.id.dataLog).getParent() : null;
            if (layout != null) {
                layout.addView(testButton, layout.getChildCount() - 1);
                layout.addView(newTestButton, layout.getChildCount() - 1);
            }
        } catch (Exception e) {
            Log.e(TAG, "Error adding test button", e);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);

        if (requestCode == 1001) { // Location permission
            if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                checkPermissionsAndStart();
            } else {
                Toast.makeText(this, R.string.location_permission_required, Toast.LENGTH_LONG).show();
            }
        } else if (requestCode == 1002) { // Bluetooth permission
            if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                checkPermissionsAndStart();
            } else {
                Toast.makeText(this, R.string.bluetooth_permission_required, Toast.LENGTH_LONG).show();
            }
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, android.content.Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == 1003) { // Bluetooth enable
            if (resultCode == RESULT_OK) {
                checkPermissionsAndStart();
            } else {
                Toast.makeText(this, R.string.enable_bluetooth, Toast.LENGTH_LONG).show();
            }
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        try {
            if (bluetoothManager != null) {
                bluetoothManager.disconnect();
            }
            parseExecutor.shutdownNow();
            Log.d(TAG, "MainActivity destroyed");
        } catch (Exception e) {
            Log.e(TAG, "Error in onDestroy", e);
        }
    }
}