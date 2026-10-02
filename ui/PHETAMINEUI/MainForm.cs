// MainForm.cs — the whole UI: find a client, inject, stream logs, run scripts.
//
// Design constraints this file obeys (they are acceptance criteria, not taste):
//   * it never builds a payload — no Lua, no bytecode, no container;
//   * it never guesses an offset or a capability: the DLL reports both, the UI
//     displays them;
//   * it never blocks the reader thread: every event marshals to the UI thread;
//   * a failed stage is shown with its name (the DLL sends "ERROR:<stage> ..."),
//     because "nothing happened" is the one answer this tool must never give.
using System.Diagnostics;
using System.Text;
using System.Text.Json;

namespace PhetamineUI;

internal sealed class MainForm : Form
{
    private readonly ComboBox _processes = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 420 };
    private readonly Button _refresh = new() { Text = "Refresh", AutoSize = true };
    private readonly Button _inject = new() { Text = "Inject", AutoSize = true };
    private readonly Label _clientInfo = new() { AutoSize = true, ForeColor = SystemColors.GrayText };

    private readonly RichTextBox _log = new()
    {
        ReadOnly = true,
        Dock = DockStyle.Fill,
        BackColor = Color.FromArgb(20, 20, 24),
        ForeColor = Color.Gainsboro,
        Font = new Font("Consolas", 9.5f),
        WordWrap = false,
        DetectUrls = false,
    };

    private readonly TextBox _script = new()
    {
        Multiline = true,
        Dock = DockStyle.Fill,
        ScrollBars = ScrollBars.Both,
        WordWrap = false,
        Font = new Font("Consolas", 10f),
        Text = "-- PHETAMINE\nprint(\"hello from the client process\")\nprint(\"identity:\", getidentity())",
    };

    private readonly Button _execute = new() { Text = "EXECUTE  (Ctrl+Enter)", AutoSize = true };
    private readonly Button _stop = new() { Text = "STOP SCRIPTS", AutoSize = true };
    private readonly Button _unload = new() { Text = "UNLOAD", AutoSize = true };
    private readonly Button _ping = new() { Text = "PING", AutoSize = true };
    private readonly CheckedListBox _capabilities = new() { Dock = DockStyle.Fill, CheckOnClick = false };
    private readonly StatusStrip _status = new();
    private readonly ToolStripStatusLabel _statusLabel = new("not attached");

    private PipeClient? _client;
    private int _nextId = 1;
    private readonly HashSet<int> _pending = [];

    public MainForm()
    {
        Text = "PHETAMINE — native internal executor (Debug build)";
        Width = 1180;
        Height = 760;
        MinimumSize = new Size(900, 600);
        Font = new Font("Segoe UI", 9f);

        // ---- layout ----
        var header = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 34, Padding = new Padding(6, 4, 6, 0) };
        header.Controls.Add(new Label { Text = "Client:", AutoSize = true, Padding = new Padding(0, 6, 4, 0) });
        header.Controls.Add(_processes);
        header.Controls.Add(_refresh);
        header.Controls.Add(_inject);
        header.Controls.Add(_clientInfo);

        var actions = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 34, Padding = new Padding(6, 2, 6, 0) };
        actions.Controls.Add(_execute);
        actions.Controls.Add(_stop);
        actions.Controls.Add(_unload);
        actions.Controls.Add(_ping);

        var bottom = new SplitContainer
        {
            Dock = DockStyle.Fill,
            Orientation = Orientation.Vertical,
            SplitterDistance = 860,
        };
        var logGroup = new GroupBox { Text = "Client log", Dock = DockStyle.Fill };
        logGroup.Controls.Add(_log);
        var capabilityGroup = new GroupBox { Text = "Capabilities (reported by the DLL)", Dock = DockStyle.Fill };
        capabilityGroup.Controls.Add(_capabilities);
        bottom.Panel1.Controls.Add(logGroup);
        bottom.Panel2.Controls.Add(capabilityGroup);
        _capabilities.Items.Add("(waiting for the Canary frame)");

        var editor = new GroupBox { Text = "Script", Dock = DockStyle.Bottom, Height = 220 };
        editor.Controls.Add(_script);

        _status.Items.Add(_statusLabel);
        Controls.Add(bottom);
        Controls.Add(editor);
        Controls.Add(actions);
        Controls.Add(header);
        Controls.Add(_status);

        // ---- events ----
        _refresh.Click += (_, _) => RefreshProcesses();
        _inject.Click += (_, _) => RunInjection();
        _execute.Click += (_, _) => ExecuteScript();
        _stop.Click += (_, _) => Guarded(() => _client?.Stop());
        _unload.Click += (_, _) => Guarded(() => _client?.Unload());
        _ping.Click += (_, _) => Guarded(() => _client?.Ping());
        _processes.SelectedIndexChanged += (_, _) => ShowSelectedClient();
        KeyPreview = true;
        KeyDown += (_, e) =>
        {
            if (e.Control && e.KeyCode == Keys.Enter) { ExecuteScript(); e.Handled = true; }
        };
        FormClosing += (_, _) => _client?.Dispose();

        RefreshProcesses();
    }

    // ---- client discovery ----------------------------------------------------
    private void RefreshProcesses()
    {
        _processes.Items.Clear();
        List<Process> clients = Injector.FindClients();
        if (clients.Count == 0)
        {
            _processes.Items.Add("(no RobloxPlayerBeta.exe running)");
            _processes.Enabled = false;
            _inject.Enabled = false;
            _statusLabel.Text = "no client";
            Append("info", "no RobloxPlayerBeta.exe process found — start the game first");
            return;
        }
        _processes.Enabled = true;
        _inject.Enabled = true;
        foreach (Process client in clients)
        {
            _processes.Items.Add($"PID {client.Id} — {Injector.ClientVersion(client)}");
        }
        _processes.SelectedIndex = 0;
    }

    private Process? SelectedClient()
    {
        List<Process> clients = Injector.FindClients();
        if (_processes.SelectedIndex < 0 || _processes.SelectedIndex >= clients.Count) return null;
        return clients[_processes.SelectedIndex];
    }

    private void ShowSelectedClient()
    {
        Process? client = SelectedClient();
        if (client is null) { _clientInfo.Text = string.Empty; return; }
        _clientInfo.Text = $"{client.ProcessName} · {Injector.ClientVersion(client)}";
    }

    // ---- injection -----------------------------------------------------------
    private void RunInjection()
    {
        Process? client = SelectedClient();
        if (client is null) { Append("error", "select a client first"); return; }

        Append("info", $"injecting into PID {client.Id} ({Injector.ClientVersion(client)})");
        InjectResult result;
        try
        {
            result = Injector.Inject(client, Injector.DefaultWorkspace);
        }
        catch (Exception ex)
        {
            Append("error", $"injection threw: {ex.Message}");
            return;
        }
        Append(result.Ok ? "info" : "error", result.Message);
        _statusLabel.Text = result.Ok ? "injected — waiting for the pipe" : "injection failed";
        if (result.Ok) Attach(client.Id);
    }

    private void Attach(int pid)
    {
        try
        {
            _client = PipeClient.Connect(pid);
        }
        catch (Exception ex)
        {
            Append("error", ex.Message);
            _statusLabel.Text = "injected but not attached";
            return;
        }

        _client.FrameReceived += OnFrame;
        _client.Disconnected += message => BeginInvoke(() =>
        {
            Append("warn", $"pipe closed: {message}");
            _statusLabel.Text = "detached";
        });
        _statusLabel.Text = $"attached to {_client.PipeName}";
        Append("info", $"attached to {_client.PipeName}");
        _client.RequestCapabilities();
    }

    // ---- frames --------------------------------------------------------------
    private void OnFrame(Op op, string text)
    {
        BeginInvoke(() =>
        {
            switch (op)
            {
                case Op.Log:
                    Append("log", text);
                    break;
                case Op.Ready:
                case Op.Info:
                    Append("info", $"session: {text}");
                    ParseCapabilities(text);
                    break;
                case Op.Canary:
                    Append("info", $"canary: {text}");
                    ParseCapabilities(Json.Child(text, "capabilities"));
                    break;
                case Op.ExecResult:
                    HandleExecResult(text);
                    break;
                default:
                    Append("log", $"[{op}] {text}");
                    break;
            }
        });
    }

    private void ParseCapabilities(string? json)
    {
        if (string.IsNullOrWhiteSpace(json)) return;
        try
        {
            using JsonDocument document = JsonDocument.Parse(json);
            if (document.RootElement.ValueKind != JsonValueKind.Object) return;
            _capabilities.Items.Clear();
            foreach (JsonProperty property in document.RootElement.EnumerateObject())
            {
                bool enabled = property.Value.ValueKind == JsonValueKind.True;
                _capabilities.Items.Add($"{property.Name}: {(enabled ? "on" : "off")}", enabled);
            }
        }
        catch (JsonException)
        {
            // A capability blob we cannot parse is not worth an error dialog; the
            // raw frame is already in the log.
        }
    }

    private void HandleExecResult(string text)
    {
        int id = Json.Int(text, "id", 0);
        bool ok = Json.Bool(text, "ok", false);
        string message = Json.String(text, "message") ?? text;
        _pending.Remove(id);
        Append(ok ? "ok" : "error", $"job {id}: {message}");

        if (ok && _pending.Count == 0 && message.Contains("queued", StringComparison.OrdinalIgnoreCase))
        {
            _statusLabel.Text = "executing…";
        }
    }

    // ---- actions -------------------------------------------------------------
    private void ExecuteScript()
    {
        if (_client is null) { Append("error", "not attached — inject first"); return; }
        string source = _script.Text;
        if (string.IsNullOrWhiteSpace(source)) { Append("error", "nothing to execute"); return; }

        int id = _nextId++;
        _pending.Add(id);
        Guarded(() =>
        {
            _client.Execute(id, source);
            _statusLabel.Text = $"sent job {id}";
        });
    }

    private void Guarded(Action action)
    {
        try
        {
            action();
        }
        catch (Exception ex)
        {
            Append("error", ex.Message);
        }
    }

    private void Append(string kind, string message)
    {
        Color colour = kind switch
        {
            "error" => Color.FromArgb(255, 120, 120),
            "warn" => Color.FromArgb(255, 200, 120),
            "ok" => Color.FromArgb(140, 220, 140),
            "info" => Color.FromArgb(130, 180, 255),
            _ => Color.Gainsboro,
        };
        _log.SelectionStart = _log.TextLength;
        _log.SelectionLength = 0;
        _log.SelectionColor = colour;
        _log.AppendText($"{DateTime.Now:HH:mm:ss} {message}{Environment.NewLine}");
        _log.SelectionColor = _log.ForeColor;
        _log.ScrollToCaret();

        // The UI's own log is a debugging aid; the client's log is the truth.
        Debug.WriteLine($"[{kind}] {message}");
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) _client?.Dispose();
        base.Dispose(disposing);
    }
}

/// <summary>
/// Tolerant JSON helpers: frames come from another process, so "the field is
/// missing" is normal (a partially written frame should not throw inside a UI
/// event handler) and must never crash the reader.
/// </summary>
internal static class Json
{
    public static string? Child(string text, string name)
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(text);
            return document.RootElement.TryGetProperty(name, out JsonElement child)
                ? child.GetRawText()
                : null;
        }
        catch (JsonException) { return null; }
    }

    public static string? String(string text, string name)
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(text);
            return document.RootElement.TryGetProperty(name, out JsonElement value) &&
                   value.ValueKind == JsonValueKind.String
                ? value.GetString()
                : null;
        }
        catch (JsonException) { return null; }
    }

    public static int Int(string text, string name, int fallback)
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(text);
            return document.RootElement.TryGetProperty(name, out JsonElement value) &&
                   value.ValueKind == JsonValueKind.Number
                ? value.GetInt32()
                : fallback;
        }
        catch (JsonException) { return fallback; }
    }

    public static bool Bool(string text, string name, bool fallback)
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(text);
            return document.RootElement.TryGetProperty(name, out JsonElement value)
                ? value.ValueKind == JsonValueKind.True
                : fallback;
        }
        catch (JsonException) { return fallback; }
    }
}
