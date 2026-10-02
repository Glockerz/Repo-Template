using System.Diagnostics;

namespace PhetamineUI;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        ApplicationConfiguration.Initialize();

        // One unhandled exception in the UI must not look like "the executor is
        // broken" — it is shown, and the process keeps its log.
        Application.ThreadException += (_, e) => ShowFatal(e.Exception);
        AppDomain.CurrentDomain.UnhandledException += (_, e) => ShowFatal(e.ExceptionObject as Exception);

        Application.Run(new MainForm());
    }

    private static void ShowFatal(Exception? exception)
    {
        string message = exception?.ToString() ?? "unknown error";
        Debug.WriteLine("PHETAMINEUI: " + message);
        MessageBox.Show(message, "PHETAMINE — UI error", MessageBoxButtons.OK, MessageBoxIcon.Error);
    }
}
