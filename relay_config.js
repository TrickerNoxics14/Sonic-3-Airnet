// Address of the relay server for online play in the browser (browsers have no UDP, so they play through a relay).
// Whoever hosts this page puts the address of a relay server here ("wss://relay.example.org"). Left empty, online play stays off until a player
// types an address under RELAY SERVER in the lobby, or opens the page as sonic3air_web.html?relay=wss://relay.example.org (the link wins over this file).
// How to run a relay: checks/relay_ws/README.md in the source repository.
window.S3AIR_RELAY_DEFAULT = '';
