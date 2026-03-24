FROM openresty/openresty:alpine

RUN apk add --no-cache curl \
    && opm get ledgetech/lua-resty-http

COPY nginx.conf /usr/local/openresty/nginx/conf/nginx.conf
COPY lua/       /usr/local/openresty/nginx/lua/

EXPOSE 80

CMD ["openresty", "-g", "daemon off;"]
