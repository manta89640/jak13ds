(AI-assisted) Host measurement of the 3DS renderer's draws and fill per bucket (soft GPU backend).
Build gk for the host, make a project mirror whose out/jak1/c3l holds the .c3l files and whose
config.ini is the user's (auto_detail = off), then:
  CTR_SOFT_STATS=60 [CTR_SOFT_MERC_MODELS=1] OPENGOAL_PAD_SCRIPT=views3.pad \
    gk --proj-path <mirror> --ctr-gfx --ctr-dump <dir> --ctr-dump-every 60 -- -boot -fakeiso -cbackend
Every 60 frames a "[softstats]" block: per bucket (level split per chunk kind/tier, merc per model
with CTR_SOFT_MERC_MODELS) draws, triangles, fragments (px, in screens of 96000), fragments after
the depth test, blended ones, nearly invisible ones (alpha < 2/128 with blending), texture changes,
shader program changes; od_*.png = fragments per pixel (red: 8+). summ.py / agg.py summarize a log.
