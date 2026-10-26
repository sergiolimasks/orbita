FROM python:3.12-slim AS base
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 PIP_NO_CACHE_DIR=1 PIP_DISABLE_PIP_VERSION_CHECK=1
WORKDIR /app
COPY requirements.txt .
RUN pip install -r requirements.txt
COPY xc360site ./xc360site
# uid fixo (antes: dono do diretório do keys.tsv da ponte, aposentado)
RUN useradd -u 10001 -M -s /usr/sbin/nologin app

# estágio de testes: docker build --target test ... && docker run ... pytest
FROM base AS test
RUN pip install pytest==8.4.2
COPY tests ./tests
USER app
CMD ["python", "-m", "pytest", "-q", "tests"]

FROM base AS prod
USER app
EXPOSE 8000
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
  CMD python -c "import urllib.request,sys; sys.exit(0 if urllib.request.urlopen('http://127.0.0.1:8000/healthz', timeout=4).status == 200 else 1)"
CMD ["gunicorn", "-w", "2", "--threads", "4", "-b", "0.0.0.0:8000", "--forwarded-allow-ips", "*", \
     "--access-logfile", "-", "--timeout", "120", "xc360site:create_app()"]
