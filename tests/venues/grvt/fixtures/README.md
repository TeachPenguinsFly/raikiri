# grvt fixtures

Captured from production `market-data.grvt.io` on 2026-09-24 over public
endpoints, each frame written unchanged. `rest_all_instruments.json` is
trimmed to four rows. The `ws_book_d_*` files are one subscription in order:
the ack's `latest_sequence_number` is the first delta's `prev_sequence_number`.
