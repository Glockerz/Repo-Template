// PipeClient.cs — the UI's half of the IPC protocol.
//
// The opcode table here is checked against src/ipc/protocol.h by
// tools/verify.mjs (name AND value). Do not "improve" a value or a name without
// changing both files: a mismatch produces a pipe that connects and then
// misbehaves silently, which is the worst failure mode this tool has.
//
// This class owns exactly one connection and one reader thread. It never builds
// a payload, never parses Luau, and never knows an offset — it is a dumb pipe.
using System.Text;

namespace PhetamineUI;

internal enum Op : byte
{
    // Direction: none of these are guesses — see the table in protocol.h.
    Ready = 0x01,        // S→U: handshake complete
    Info = 0x02,         // S→U: placeId/jobId/user
    Log = 0x03,          // S→U: script + engine log
    Canary = 0x04,       // S→U: capability bitmap
    ExecResult = 0x05,   // S→U: {id, ok, message}
    Execute = 0x10,      // U→S: run source
    Stop = 0x11,         // U→S: retire script threads
    Unload = 0x12,       // U→S: clean shutdown
    Ping = 0x13,         // U→S: liveness probe
    Capabilities = 0x14, // U→S: request the bitmap
}

internal sealed record PipeFrame(Op Op, byte[] Payload)
{
    public string Text => Encoding.UTF8.GetString(Payload);
}

/// <summary>One client, one pipe, one reader thread.</summary>
internal sealed class PipeClient : IDisposable
{
    private const int MaxFrame = 8 * 1024 * 1024;
    private const int ConnectTimeoutMs = 5000;

    private readonly FileStream _stream;
    private readonly Thread _reader;
    private volatile bool _running = true;

    public event Action<Op, string>? FrameReceived;
    public event Action<string>? Disconnected;

    public string PipeName { get; }
    public bool Connected => _running;

    private PipeClient(string pipeName, FileStream stream)
    {
        PipeName = pipeName;
        _stream = stream;
        _reader = new Thread(ReadLoop) { IsBackground = true, Name = "PHETAMINE pipe reader" };
        _reader.Start();
    }

    /// <summary>
    /// Connects to \\.\pipe\PHETAMINE_&lt;pid&gt;. The pipe is created by the DLL at
    /// init, so a timeout here means "injected but not up yet", and the caller
    /// should retry rather than re-inject.
    /// </summary>
    public static PipeClient Connect(int pid, int timeoutMs = ConnectTimeoutMs)
    {
        string name = $"PHETAMINE_{pid}";
        string path = $@"\\.\pipe\{name}";
        DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
        Exception? last = null;

        while (DateTime.UtcNow < deadline)
        {
            try
            {
                var stream = new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.ReadWrite);
                return new PipeClient(name, stream);
            }
            catch (Exception ex)
            {
                last = ex;
                Thread.Sleep(150);
            }
        }

        throw new IOException(
            $"could not connect to {path} within {timeoutMs} ms ({last?.Message}). " +
            "The module is either not injected or did not finish initialising — check the log pane " +
            "and the ERROR:<stage> line.");
    }

    private void ReadLoop()
    {
        byte[] lengthBuffer = new byte[4];
        try
        {
            while (_running)
            {
                if (!ReadExact(lengthBuffer, 4)) break;
                int length = BitConverter.ToInt32(lengthBuffer, 0);
                if (length < 1 || length > MaxFrame) break;
                byte[] payload = new byte[length];
                if (!ReadExact(payload, length)) break;
                Op op = (Op)payload[0];
                byte[] body = payload[1..];
                FrameReceived?.Invoke(op, Encoding.UTF8.GetString(body));
            }
        }
        catch (Exception ex)
        {
            if (_running) Disconnected?.Invoke(ex.Message);
            return;
        }
        _running = false;
        Disconnected?.Invoke("the module closed the pipe (unloaded, or the client exited)");
    }

    private bool ReadExact(byte[] buffer, int count)
    {
        int offset = 0;
        while (offset < count)
        {
            int read = _stream.Read(buffer, offset, count - offset);
            if (read <= 0) return false;
            offset += read;
        }
        return true;
    }

    public void Send(Op op, byte[] payload)
    {
        if (!_running) throw new IOException("the pipe is closed");
        byte[] frame = new byte[5 + payload.Length];
        BitConverter.GetBytes(1 + payload.Length).CopyTo(frame, 0);
        frame[4] = (byte)op;
        payload.CopyTo(frame, 5);
        lock (_stream)
        {
            _stream.Write(frame, 0, frame.Length);
            _stream.Flush();
        }
    }

    public void SendText(Op op, string text) => Send(op, Encoding.UTF8.GetBytes(text));

    // ---- the four things the UI actually does --------------------------------
    // Execute payload: [u32 id][utf8 source]. The id is echoed in ExecResult so a
    // script that takes seconds to compile cannot mislead the log.
    public void Execute(int id, string source)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(source);
        byte[] payload = new byte[4 + bytes.Length];
        BitConverter.GetBytes(id).CopyTo(payload, 0);
        bytes.CopyTo(payload, 4);
        Send(Op.Execute, payload);
    }

    public void Stop() => Send(Op.Stop, []);
    public void Unload() => Send(Op.Unload, []);
    public void Ping() => Send(Op.Ping, []);
    public void RequestCapabilities() => Send(Op.Capabilities, []);

    public void Dispose()
    {
        _running = false;
        try { _stream.Dispose(); } catch { /* already gone */ }
        if (_reader.IsAlive) _reader.Join(500);
    }
}
