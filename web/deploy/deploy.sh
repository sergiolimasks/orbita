#!/bin/sh
# Rodar no devbox, a partir da raiz do repo:  sh web/deploy/deploy.sh [--test-only]
# Copia web/ pra VPS, builda a imagem lá (como as outras imagens locais), roda
# os testes contra um Postgres descartável e, se passarem, atualiza a stack.
set -eu
VPS=${VPS:-cc360-vps}
DIR=/root/xc360-site
TAG=$(date -u +%Y%m%d-%H%M%S)
cd "$(dirname "$0")/.."

tar --exclude=__pycache__ --exclude=.pytest_cache -czf - . | ssh "$VPS" "rm -rf $DIR/src && mkdir -p $DIR/src && tar -xzf - -C $DIR/src"

ssh "$VPS" TAG="$TAG" DIR="$DIR" TEST_ONLY="${1:-}" 'sh -s' <<'EOF'
set -eu
cd "$DIR/src"
docker build -q --target test -t xc360-site-test:$TAG . >/dev/null
docker build -q --target prod -t xc360-site:$TAG . >/dev/null
# Postgres descartável numa rede isolada só pros testes
NET=xc360test-$TAG
docker network create "$NET" >/dev/null
docker run -d --rm --name pg-$TAG --network "$NET" -e POSTGRES_PASSWORD=t -e POSTGRES_DB=t postgres:16-alpine >/dev/null
trap 'docker rm -f pg-$TAG >/dev/null 2>&1; docker network rm $NET >/dev/null 2>&1; docker rmi xc360-site-test:$TAG >/dev/null 2>&1' EXIT
for i in $(seq 1 30); do docker exec pg-$TAG pg_isready -U postgres -q && break; sleep 1; done
sleep 2
docker run --rm --network "$NET" -e TEST_DATABASE_URL=postgresql://postgres:t@pg-$TAG:5432/t xc360-site-test:$TAG \
  python -m pytest -q -p no:cacheprovider tests
[ "$TEST_ONLY" = "--test-only" ] && exit 0
cp deploy/stack.yml "$DIR/stack.yml"
TAG=$TAG docker stack deploy -c "$DIR/stack.yml" xc360-site --with-registry-auth >/dev/null
echo "$TAG" > "$DIR/CURRENT_TAG"
echo "deploy xc360-site:$TAG ok"
EOF
