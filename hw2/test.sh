#!/bin/bash

TEST_STRING="Hello Kernel!"
HEX="$(printf '%s' "$TEST_STRING" | hexdump -ve '/1 "%02X"' | sed 's/../& /g;s/ $//' | tr 'A-F' 'a-f')"

echo "Запись тестовой строки:"
echo "    echo $TEST_STRING | sudo tee /dev/nulldump > /dev/null"
echo $TEST_STRING | sudo tee /dev/nulldump > /dev/null

sleep 1

echo "Проверка вывода в dmesg:"
DMESG_OUTPUT=$(sudo dmesg | tail -n 1)
echo "    $DMESG_OUTPUT"

if echo "$DMESG_OUTPUT" | grep -q "$HEX.*$TEST_STRING"; then
    echo "Тест пройден!"
else
    echo "Тест провален."
    exit 1
fi