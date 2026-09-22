#!/usr/bin/env python3
"""
Sincronizador de Canais IPTV
Gera channels.json a partir das listas validadas brazil_iptv_working.json
e working-premium.json (filtrando apenas canais 720p+).
"""

import os
import json

CONFIG_DIR = os.path.dirname(os.path.abspath(__file__))
CHANNELS_FILE = os.path.join(CONFIG_DIR, "channels.json")
BRAZIL_FILE = os.path.join(CONFIG_DIR, "brazil_iptv_working.json")
PREMIUM_FILE = os.path.join(CONFIG_DIR, "working-premium.json")

META = {
    # TV Aberta (13)
    "tv-gazeta-sp": {"name": "TV Gazeta SP", "quality": "1080p", "category": "TV Aberta", "logo": "📺"},
    "globo-morena-dourados": {"name": "Rede Globo (TV Morena)", "quality": "720p", "category": "TV Aberta", "logo": "🌐"},
    "rede-tv-nacional": {"name": "RedeTV! Nacional", "quality": "720p", "category": "TV Aberta", "logo": "📺"},
    "redetv-parana": {"name": "RedeTV! Paraná", "quality": "1080p", "category": "TV Aberta", "logo": "📺"},
    "rede-brasil": {"name": "Rede Brasil (RBTV)", "quality": "1080p", "category": "TV Aberta", "logo": "📺"},
    "rede-ngt": {"name": "Rede NGT", "quality": "1080p", "category": "TV Aberta", "logo": "📺"},
    "cnt-rio": {"name": "Rede CNT Rio", "quality": "720p", "category": "TV Aberta", "logo": "📺"},
    "sbt-interior": {"name": "SBT Interior", "quality": "720p", "category": "TV Aberta", "logo": "📺"},
    "tv-cultura-sp": {"name": "TV Cultura", "quality": "720p", "category": "TV Aberta", "logo": "🏛️"},
    "terraviva": {"name": "Terra Viva (Band Agro)", "quality": "720p", "category": "TV Aberta", "logo": "🌱"},
    "tv-a-critica": {"name": "TV A Crítica (Manaus)", "quality": "1080p", "category": "TV Aberta", "logo": "📺"},
    "amazon-sat": {"name": "Amazon Sat", "quality": "1080p", "category": "TV Aberta", "logo": "🌳"},
    "tcm10-hd": {"name": "TCM 10 HD", "quality": "1080p", "category": "TV Aberta", "logo": "📺"},

    # Notícias (4)
    "record-news": {"name": "Record News", "quality": "1080p", "category": "Notícias", "logo": "📰"},
    "record-news-pluto": {"name": "Record News (FAST)", "quality": "720p", "category": "Notícias", "logo": "📰"},
    "sbt-news": {"name": "SBT News", "quality": "720p", "category": "Notícias", "logo": "📰"},
    "canal-uol": {"name": "Canal UOL", "quality": "720p", "category": "Notícias", "logo": "📰"},

    # Filmes & Séries (6)
    "studio-universal-br": {"name": "Studio Universal", "quality": "1080p", "category": "Filmes & Séries", "logo": "🎬"},
    "axn-brasil": {"name": "AXN Brasil", "quality": "720p", "category": "Filmes & Séries", "logo": "💥"},
    "sony-channel-br": {"name": "Sony Channel", "quality": "720p", "category": "Filmes & Séries", "logo": "🍿"},
    "tnt-novelas-br": {"name": "TNT Novelas", "quality": "720p", "category": "Filmes & Séries", "logo": "🎭"},
    "ae-latin-br": {"name": "A&E Brasil", "quality": "720p", "category": "Filmes & Séries", "logo": "🔍"},
    "loading-tv": {"name": "Loading TV", "quality": "720p", "category": "Filmes & Séries", "logo": "🎮"},

    # Esportes (3)
    "sportv3-mirror": {"name": "SporTV 3", "quality": "720p", "category": "Esportes", "logo": "⚽"},
    "espn-mirror-a07z": {"name": "ESPN", "quality": "1080p", "category": "Esportes", "logo": "🏈"},
    "espn4-mirror-a07n": {"name": "ESPN 4", "quality": "1080p", "category": "Esportes", "logo": "🎾"},

    # TV Pública / Educativa (8)
    "tv-brasil-ebc": {"name": "TV Brasil (EBC)", "quality": "720p", "category": "TV Pública / Educativa", "logo": "🏛️"},
    "tv-camara": {"name": "TV Câmara", "quality": "1080p", "category": "TV Pública / Educativa", "logo": "⚖️"},
    "tv-senado": {"name": "TV Senado", "quality": "HD", "category": "TV Pública / Educativa", "logo": "⚖️"},
    "tv-justica": {"name": "TV Justiça (STF)", "quality": "720p", "category": "TV Pública / Educativa", "logo": "⚖️"},
    "tve-rs": {"name": "TVE Rio Grande do Sul", "quality": "1080p", "category": "TV Pública / Educativa", "logo": "🏛️"},
    "tve-bahia-mirror": {"name": "TVE Bahia", "quality": "1080p", "category": "TV Pública / Educativa", "logo": "🏛️"},
    "canal-futura": {"name": "Canal Futura (Globo)", "quality": "720p", "category": "TV Pública / Educativa", "logo": "🎓"},
    "tv-ufop": {"name": "TV UFOP", "quality": "1080p", "category": "TV Pública / Educativa", "logo": "🎓"},

    # Infantil (1)
    "nick-classico-pluto": {"name": "Nickelodeon Clássico", "quality": "720p", "category": "Infantil", "logo": "🧸"},

    # Religiosos (6)
    "tv-aparecida": {"name": "TV Aparecida", "quality": "720p", "category": "Religiosos", "logo": "🙏"},
    "rede-vida": {"name": "Rede Vida", "quality": "720p", "category": "Religiosos", "logo": "🙏"},
    "tv-cancao-nova": {"name": "TV Canção Nova", "quality": "720p", "category": "Religiosos", "logo": "🙏"},
    "rit-tv": {"name": "RIT TV", "quality": "1080p", "category": "Religiosos", "logo": "🙏"},
    "rede-gospel": {"name": "Rede Gospel", "quality": "1080p", "category": "Religiosos", "logo": "🙏"},
    "tv-mana-brasil": {"name": "TV Maná Brasil", "quality": "1080p", "category": "Religiosos", "logo": "🙏"},
}

def sync():
    print("[*] Sincronizando canais validados...")
    with open(BRAZIL_FILE, "r", encoding="utf-8") as f:
        br_channels = json.load(f)["channels"]

    with open(PREMIUM_FILE, "r", encoding="utf-8") as f:
        prem_channels = [c for c in json.load(f)["channels"] if c.get("resolution_label") != "480p"]

    all_src = {c["id"]: c for c in br_channels + prem_channels}
    final_channels = {}
    for cid in META.keys():
        if cid in all_src:
            src = all_src[cid]
            meta = META[cid]
            final_channels[cid] = {
                "id": cid,
                "name": meta["name"],
                "quality": meta["quality"],
                "category": meta["category"],
                "url": src["url"],
                "logo": meta["logo"],
                "description": src.get("description", "")
            }

    with open(CHANNELS_FILE, "w", encoding="utf-8") as f:
        json.dump(final_channels, f, indent=2, ensure_ascii=False)

    print(f"[✓] channels.json atualizado com {len(final_channels)} canais verificados.")
    return final_channels

if __name__ == "__main__":
    sync()
