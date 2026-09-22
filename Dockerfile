FROM python:3.11-slim

# Install ffmpeg and required utilities
RUN apt-get update && apt-get install -y --no-install-recommends \
    ffmpeg \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy application files
COPY server.py ./
COPY channels_deploy.json ./
COPY channels.json ./

# Default environment variables
ENV HOST=0.0.0.0 \
    PORT=8080 \
    PYTHONUNBUFFERED=1 \
    TV_PIN=1233 \
    STANDBY_TIMEOUT=90.0 \
    CHANNELS_FILE=/app/channels_deploy.json \
    MOVIES_DIR=/app/filmes

EXPOSE 8080

CMD ["python3", "server.py"]
