require("serverquery")

serverquery.SetPlayerSpoofing(true)
serverquery.SetPlayerCount(20)

serverquery.ResetPlayers()
serverquery.AddPlayer("Omar", 10, 300)
serverquery.AddPlayer("Alex", 20, 100)

serverquery.SetMapDetection(false)
serverquery.SetMapName("rp_cometpingpong")