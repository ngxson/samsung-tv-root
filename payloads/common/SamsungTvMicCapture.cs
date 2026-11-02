// Samsung Smart Control remote-microphone capture / TV-side voice trigger.
//
// Commands the paired Bluetooth remote to open its microphone and stream
// audio to the TV (bt_hid_rc_start_sending_voice), receives the audio
// directly through the Bluetooth CAPI callback, and writes it to a PCM file.
//
// No voice assistant is involved: the STT/Bixby stack is never started and
// no voice services need to be configured on the TV — no Bixby login needed.
//
// IMPORTANT (learned from REMOTE_PROTO.md §6.1): the remote only accepts the
// start command while its MIC button is physically held (a ~1 s window opens
// on the key press and closes on the all-keys-released path). The remote
// silently drops `start` outside that window. Therefore this payload RETRIES
// bt_hid_rc_start_sending_voice every 300 ms for the whole run: keep the MIC
// button pressed any time during the run and the stream should arm. While
// audio is arriving it switches to extend calls (bt_hid_rc_extend_sending_voice)
// to keep the session alive.
//
// Usage: dotnet SamsungTvMicCapture.dll [seconds] [output.pcm]
// The PCM is 16 kHz mono signed 16-bit little-endian.
// A sidecar <output.pcm>.log records per-chunk timing (cadence), sizes and
// the first bytes of the first chunks — the raw frame header before decode.

using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

internal static class SamsungTvMicCapture
{
    [StructLayout(LayoutKind.Sequential)]
    private struct VoiceData
    {
        public short length;
        private readonly short padding;
        public IntPtr audioBuffer;
    }

    // void cb(bt_hid_voice_data_s* data, void* user_data)
    private delegate void AudioDataReceiveCallback(ref VoiceData data, IntPtr userData);

    private const string PlainLibrary = "libcapi-network-bluetooth.so.0.5.3";
    private const string TvLibrary = "libcapi-network-bluetooth-tv.so.0.1.04_tv";

    [DllImport(PlainLibrary)]
    private static extern int bt_audio_initialize();

    [DllImport(PlainLibrary)]
    private static extern int bt_audio_deinitialize();

    [DllImport(PlainLibrary)]
    private static extern int bt_hid_host_initialize();

    [DllImport(PlainLibrary)]
    private static extern int bt_hid_host_deinitialize();

    [DllImport(PlainLibrary)]
    private static extern int bt_initialize();

    [DllImport(PlainLibrary)]
    private static extern int bt_deinitialize();

    [DllImport(TvLibrary, EntryPoint = "get_size_of_bt_hid_voice_data_s")]
    private static extern int GetVoiceDataSize();

    [DllImport(TvLibrary, EntryPoint = "bt_hid_get_smart_rc_address")]
    private static extern int GetSmartRcAddress(out IntPtr address);

    [DllImport(TvLibrary, EntryPoint = "bt_hid_get_smart_rc_device_type")]
    private static extern int GetSmartRcDeviceType(out int deviceType);

    [DllImport(TvLibrary, EntryPoint = "bt_hid_rc_start_sending_voice")]
    private static extern int StartSendingVoice([MarshalAs(UnmanagedType.LPStr)] string address);

    [DllImport(TvLibrary, EntryPoint = "bt_hid_rc_extend_sending_voice")]
    private static extern int ExtendSendingVoice([MarshalAs(UnmanagedType.LPStr)] string address);

    [DllImport(TvLibrary, EntryPoint = "bt_hid_rc_stop_sending_voice")]
    private static extern int StopSendingVoice([MarshalAs(UnmanagedType.LPStr)] string address);

    [DllImport(TvLibrary, EntryPoint = "bt_hid_set_audio_data_receive_cb")]
    private static extern int SetAudioDataReceiveCallback(
        AudioDataReceiveCallback callback,
        IntPtr userData);

    [DllImport(TvLibrary, EntryPoint = "bt_hid_unset_audio_data_receive_cb")]
    private static extern int UnsetAudioDataReceiveCallback();

    private static readonly object Gate = new object();
    private static FileStream _sink;
    private static StreamWriter _log;
    private static long _written;
    private static long _chunks;
    private static long _firstAtMillis = -1;
    private static int _hexDumps;
    private static readonly Stopwatch Clock = Stopwatch.StartNew();
    private static readonly DateTime UnixEpoch =
        new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc);

    private static void OnAudioData(ref VoiceData data, IntPtr userData)
    {
        int length = data.length;
        if (length <= 0 || length > 65535 || data.audioBuffer == IntPtr.Zero)
        {
            return;
        }
        byte[] buffer = new byte[length];
        Marshal.Copy(data.audioBuffer, buffer, 0, length);
        lock (Gate)
        {
            long now = Clock.ElapsedMilliseconds;
            if (_firstAtMillis < 0)
            {
                _firstAtMillis = now;
            }
            _sink.Write(buffer, 0, buffer.Length);
            _written += buffer.Length;
            _chunks++;
            if (_log != null)
            {
                _log.Write(now.ToString(CultureInfo.InvariantCulture));
                _log.Write(' ');
                _log.WriteLine(length.ToString(CultureInfo.InvariantCulture));
                if (_hexDumps < 8)
                {
                    _hexDumps++;
                    StringBuilder hex = new StringBuilder();
                    int limit = Math.Min(length, 24);
                    for (int i = 0; i < limit; i++)
                    {
                        hex.Append(buffer[i].ToString("x2", CultureInfo.InvariantCulture));
                        hex.Append(' ');
                    }
                    _log.Write("  #");
                    _log.Write(_chunks.ToString(CultureInfo.InvariantCulture));
                    _log.Write(" first bytes: ");
                    _log.WriteLine(hex.ToString().TrimEnd());
                }
            }
        }
    }

    private static int Main(string[] args)
    {
        double seconds = 15.0;
        if (args.Length > 0)
        {
            if (!double.TryParse(args[0], NumberStyles.Float,
                CultureInfo.InvariantCulture, out seconds) ||
                seconds < 1 || seconds > 180)
            {
                Console.Error.WriteLine(
                    "usage: SamsungTvMicCapture [seconds 1-180] [output.pcm]");
                return 64;
            }
        }
        string output = args.Length > 1 ? args[1] : "/tmp/mic_capture.pcm";

        Console.WriteLine(
            $"struct size check: bt_hid_voice_data_s={GetVoiceDataSize()} (expect 8)");
        // The CAPI checks "is initialized" before most calls, so bt_initialize()
        // must run before the audio/HID-host initialization, exactly like the
        // production voice stack does (its app framework initializes first).
        int rc = bt_initialize();
        Console.WriteLine($"bt_initialize -> {rc}");
        rc = bt_audio_initialize();
        Console.WriteLine($"bt_audio_initialize -> {rc}");
        rc = bt_hid_host_initialize();
        Console.WriteLine($"bt_hid_host_initialize -> {rc}");

        string address = null;
        rc = GetSmartRcAddress(out IntPtr pointer);
        if (rc == 0)
        {
            address = Marshal.PtrToStringAnsi(pointer);
        }
        if (rc != 0 || String.IsNullOrEmpty(address))
        {
            Console.Error.WriteLine(
                $"bt_hid_get_smart_rc_address rc={rc}: Smart Control remote not connected?");
            return 1;
        }
        Console.WriteLine($"smart control address: {address}");
        rc = GetSmartRcDeviceType(out int deviceType);
        Console.WriteLine($"bt_hid_get_smart_rc_device_type -> {rc} type={deviceType}");

        _sink = File.Create(output);
        _log = new StreamWriter(output + ".log") { AutoFlush = true };
        _log.WriteLine(
            $"# start={DateTime.UtcNow:O} unix={(long)(DateTime.UtcNow - UnixEpoch).TotalMilliseconds}");
        _log.WriteLine("# columns: elapsed_ms chunk_length; '#' lines dump first chunk bytes");
        var callback = new AudioDataReceiveCallback(OnAudioData);
        rc = SetAudioDataReceiveCallback(callback, IntPtr.Zero);
        Console.WriteLine($"bt_hid_set_audio_data_receive_cb -> {rc}");
        if (rc != 0)
        {
            return 1;
        }

        Console.WriteLine();
        Console.WriteLine(">>> HOLD THE MIC BUTTON ON THE REMOTE NOW (LED side up).");
        Console.WriteLine(">>> The start command is retried every 300 ms; the remote");
        Console.WriteLine(">>> only accepts it while MIC is held (1 s window).");
        Console.WriteLine(">>> Keep it held and speak; release when done.");
        Console.WriteLine();

        long deadline = Clock.ElapsedMilliseconds + (long)(seconds * 1000.0);
        bool streaming = false;
        int startCalls = 0;
        int extendCalls = 0;
        int lastStatusChunk = 0;
        while (Clock.ElapsedMilliseconds < deadline)
        {
            lock (Gate)
            {
                streaming = _chunks > 0;
            }
            if (!streaming)
            {
                // Remote drops start outside the MIC window; retry until it lands.
                int start = StartSendingVoice(address);
                startCalls++;
                if (start != 0 && startCalls <= 3)
                {
                    Console.WriteLine($"bt_hid_rc_start_sending_voice -> {start}");
                }
                Thread.Sleep(300);
            }
            else
            {
                int extend = ExtendSendingVoice(address);
                if (extend == 0)
                {
                    extendCalls++;
                }
                Thread.Sleep(500);
            }
            lock (Gate)
            {
                if (_chunks != lastStatusChunk)
                {
                    lastStatusChunk = (int)_chunks;
                    Console.Write($"\rchunks {_chunks}, bytes {_written}" +
                        $" ({Clock.ElapsedMilliseconds - _firstAtMillis} ms)");
                }
            }
        }
        Console.WriteLine();

        rc = StopSendingVoice(address);
        Console.WriteLine($"bt_hid_rc_stop_sending_voice -> {rc}");
        Console.WriteLine(
            $"start calls: {startCalls}, extend calls: {extendCalls}");
        Thread.Sleep(300);
        lock (Gate)
        {
            _sink.Flush();
            _sink.Dispose();
            _log.Dispose();
        }
        UnsetAudioDataReceiveCallback();
        Console.WriteLine("bt_hid_unset_audio_data_receive_cb -> 0");
        bt_hid_host_deinitialize();
        bt_audio_deinitialize();

        Console.WriteLine(
            $"captured {_written} bytes ({_chunks} chunks) of 16 kHz mono s16le PCM in {output}");
        Console.WriteLine($"chunk log (cadence + first bytes) in {output}.log");
        return _written > 0 ? 0 : 2;
    }
}
