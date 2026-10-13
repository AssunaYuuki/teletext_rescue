// WaveTop (датакаст файлов веб-кэша через NABTS, 1998–2000): пачки по 18 пакетов одного адреса
// с поколонной защитой в GF(256), запись 416 байт (скремблер LFSR, CRC-16/ARC), сборка файлов,
// контейнер WTZY и сжатие PKWARE DCL. Работает поверх прочитанных пакетов NABTS (.t33, 33 байта после E7).
#pragma once
#include "json.h"
#include "util.h"

// t33 — пакеты; pos — рядом лежащий <t33>.pos (поле и строка каждого пакета), может отсутствовать.
// out_dir — куда класть восстановленные файлы. -> отчёт (пусто, если пачек WaveTop нет)
Json wavetop_decode(const std::string &t33, const std::string &out_dir, Progress &pr);
