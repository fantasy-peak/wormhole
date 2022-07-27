curl -k --noproxy --include \
       --no-buffer \
       --header "Connection: Upgrade" \
       --header "Upgrade: websocket" \
       --header "Host: 10.85.56.66:8855" \
       --header "Origin: http://example.com:80" \
       --header "Sec-WebSocket-Version: 13" \
       --header "Sec-WebSocket-Key: tlPf2p4JMfehB0A+6/7BdQ==" \
       https://127.0.0.1:8855/chat -v
